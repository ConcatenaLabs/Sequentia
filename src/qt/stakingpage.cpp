// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <qt/bitcoinunits.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <key.h>
#include <key_io.h>
#include <pos.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <cmath>

#include <QDoubleValidator>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

StakingPage::StakingPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* title = new QLabel(tr("Staking"), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.4);
    tf.setBold(true);
    title->setFont(tf);
    layout->addWidget(title);

    QLabel* intro = new QLabel(
        tr("Stake %1 to become a block producer. Your stake stays yours; it is time-locked only for "
           "the unbonding period you would wait before withdrawing, and it keeps counting the entire "
           "time. The more you stake, the more often the committee elects you to produce a block and "
           "collect its fees.").arg(BitcoinUnits::policyAssetTicker()), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // --- Block-production status (read from this node's own config; the GUI shares the node process) ---
    m_producer_status = new QLabel(this);
    m_producer_status->setWordWrap(true);
    m_producer_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_producer_status);
    // One-click enable: starts the autonomous producer at runtime (no restart) for the
    // staking keys this wallet controls, and persists it so it resumes after a restart.
    m_enable_button = new QPushButton(tr("Start producing blocks"), this);
    m_enable_button->setVisible(false);
    {
        QHBoxLayout* enRow = new QHBoxLayout();
        enRow->addWidget(m_enable_button);
        enRow->addStretch();
        layout->addLayout(enRow);
    }
    connect(m_enable_button, &QPushButton::clicked, this, &StakingPage::onEnableProduction);

    // --- Stake action ---
    QGroupBox* stakeGroup = new QGroupBox(tr("Stake %1").arg(BitcoinUnits::policyAssetTicker()), this);
    QFormLayout* stakeForm = new QFormLayout(stakeGroup);
    m_stake_amount = new QLineEdit(stakeGroup);
    m_stake_amount->setPlaceholderText(tr("amount of %1 (at or above the chain minimum, e.g. 50000)").arg(BitcoinUnits::policyAssetTicker()));
    {
        QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
        auto* v = new QDoubleValidator(0, 1e15, 8, m_stake_amount);
        v->setLocale(lc);
        m_stake_amount->setValidator(v);
    }
    m_stake_button = new QPushButton(tr("Stake"), stakeGroup);
    m_result = new QLabel(stakeGroup);
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stakeForm->addRow(tr("Amount:"), m_stake_amount);
    stakeForm->addRow(QString(), m_stake_button);
    stakeForm->addRow(tr("Result:"), m_result);
    layout->addWidget(stakeGroup);

    // --- Committee / registry status ---
    QGroupBox* statusGroup = new QGroupBox(tr("Stake registry"), this);
    QVBoxLayout* statusLayout = new QVBoxLayout(statusGroup);
    m_summary = new QLabel(statusGroup);
    statusLayout->addWidget(m_summary);
    m_stakers = new QTableWidget(0, 2, statusGroup);
    m_stakers->setHorizontalHeaderLabels({tr("Staker public key"), tr("Weight (%1)").arg(BitcoinUnits::policyAssetTicker())});
    m_stakers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_stakers->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_stakers->verticalHeader()->setVisible(false);
    m_stakers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    statusLayout->addWidget(m_stakers);
    m_refresh_button = new QPushButton(tr("Refresh"), statusGroup);
    QHBoxLayout* refreshRow = new QHBoxLayout();
    refreshRow->addStretch();
    refreshRow->addWidget(m_refresh_button);
    statusLayout->addLayout(refreshRow);
    layout->addWidget(statusGroup);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch();

    connect(m_stake_button, &QPushButton::clicked, this, &StakingPage::onStake);
    connect(m_refresh_button, &QPushButton::clicked, this, &StakingPage::onRefreshClicked);
}

void StakingPage::setModel(WalletModel* model)
{
    m_wallet_model = model;
    if (m_wallet_model) refresh();
}

std::string StakingPage::walletUri() const
{
    if (!m_wallet_model) return std::string();
    return "/wallet/" + m_wallet_model->getWalletName().toStdString();
}

UniValue StakingPage::callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error, bool wallet)
{
    ok = false;
    if (!m_wallet_model) { error = tr("No wallet loaded."); return UniValue(); }
    try {
        UniValue r = m_wallet_model->node().executeRpc(method, params, wallet ? walletUri() : std::string());
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

void StakingPage::setStatus(const QString& msg, bool error)
{
    m_status->setStyleSheet(error ? "color:#a00;" : "color:#070;");
    m_status->setText(msg);
}

void StakingPage::refresh()
{
    // Fetch the stake registry first; the producer banner needs it to verify that a
    // configured producer key is actually staked at/above the chain minimum.
    UniValue reg; bool haveReg = false; QString regErr;
    if (m_wallet_model) {
        bool rok;
        reg = callRpc("getstakerinfo", UniValue(UniValue::VARR), rok, regErr, /*wallet=*/false);
        haveReg = rok && reg.isObject();
    }

    // Block-production status from this node's own config (gArgs; the GUI shares the
    // node process), gated on a configured key actually holding an eligible stake.
    // Config alone does not produce blocks, so green requires on-chain eligibility.
    if (m_producer_status) {
        const bool configured = gArgs.GetBoolArg("-posproducer", false);
        const std::vector<std::string> wifs = gArgs.GetArgs("-posproducerkey");
        int eligible = 0;
        if (configured && haveReg) {
            const uint64_t floor = std::max<uint64_t>(g_pos_min_stake, 1);
            for (const std::string& w : wifs) {
                CKey key = DecodeSecret(w);
                if (!key.IsValid()) continue;
                const std::string pk = HexStr(key.GetPubKey());
                if (reg[pk].isNum() && (uint64_t)reg[pk].get_int64() >= floor) ++eligible;
            }
        }
        if (configured && !wifs.empty() && eligible > 0) {
            m_producer_status->setText(tr("Block production: ON. %1 key(s) hold an eligible stake. You produce "
                                          "automatically whenever the committee elects one of them. This resumes "
                                          "by itself after a restart.").arg(eligible));
            m_producer_status->setStyleSheet("QLabel{padding:8px;border-radius:4px;background:#e6f4ea;color:#1e7e34;}");
            if (m_enable_button) m_enable_button->setVisible(false);
        } else if (configured && !wifs.empty()) {
            m_producer_status->setText(tr("Block production: ON, waiting for your stake to count. Your producer key(s) "
                                          "are not yet registered at or above the chain minimum, or the stake has not "
                                          "confirmed yet. Stake below; you'll start producing as soon as it confirms."));
            m_producer_status->setStyleSheet("QLabel{padding:8px;border-radius:4px;background:#fff3cd;color:#856404;}");
            if (m_enable_button) m_enable_button->setVisible(false);
        } else {
            m_producer_status->setText(tr("Block production: off. Stake below and it turns on automatically, or, if you "
                                          "already have a stake, click \"Start producing blocks\". No config editing or "
                                          "restart needed."));
            m_producer_status->setStyleSheet("QLabel{padding:8px;border-radius:4px;background:#fff3cd;color:#856404;}");
            // Offer one-click enable when this wallet actually controls a registered stake.
            if (m_enable_button) m_enable_button->setVisible(!walletStakingWifs().isEmpty());
        }
    }

    if (!m_wallet_model) return;
    if (!haveReg) { m_summary->setText(tr("Stake registry unavailable: %1").arg(regErr)); return; }
    const std::vector<std::string>& keys = reg.getKeys();
    m_stakers->setRowCount(0);
    for (size_t i = 0; i < keys.size(); ++i) {
        int row = m_stakers->rowCount();
        m_stakers->insertRow(row);
        m_stakers->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(keys[i])));
        // Stake weight is atoms of the policy asset; show it in SEQ (1 SEQ = 1e8 atoms).
        const int64_t w = reg[i].isNum() ? reg[i].get_int64() : 0;
        QString seq = QString::number((double)w / 100000000.0, 'f', 8);
        if (seq.contains('.')) { while (seq.endsWith('0')) seq.chop(1); if (seq.endsWith('.')) seq.chop(1); }
        m_stakers->setItem(row, 1, new QTableWidgetItem(seq));
    }
    // Always stamp the update time: it is the only visible change when the
    // registry contents did not move between two refreshes.
    m_summary->setText(tr("%1 registered staker(s) — updated at %2.")
                           .arg(keys.size())
                           .arg(QTime::currentTime().toString(Qt::TextDate)));
}

void StakingPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void StakingPage::onRefreshClicked()
{
    // The registry RPCs are fast enough to run synchronously, so without explicit
    // feedback a click looks like a no-op whenever the list is unchanged. Show the
    // in-progress state, let the event loop paint it, then refresh (which stamps
    // the summary with the update time) and restore the button.
    if (!m_refresh_button) return;
    m_refresh_button->setEnabled(false);
    m_refresh_button->setText(tr("Refreshing…"));
    if (m_summary) m_summary->setText(tr("Refreshing the stake registry…"));
    QTimer::singleShot(100, this, [this] {
        refresh();
        m_refresh_button->setText(tr("Refresh"));
        m_refresh_button->setEnabled(true);
    });
}

void StakingPage::onStake()
{
    if (!m_wallet_model) return;
    const QString amount = m_stake_amount->text().trimmed();
    if (amount.isEmpty()) { setStatus(tr("Enter an amount of %1 to stake.").arg(BitcoinUnits::policyAssetTicker()), true); return; }
    bool amtok = false; const double amtval = amount.toDouble(&amtok);
    if (!amtok || amtval <= 0) { setStatus(tr("Enter a positive %1 amount.").arg(BitcoinUnits::policyAssetTicker()), true); return; }
    // Reject sub-floor stakes up front: the consensus rule (and registerstake) drop
    // anything below the chain minimum, so it would never count toward production.
    if (g_pos_min_stake > 0) {
        const int64_t amt_sats = (int64_t)std::llround(amtval * 100000000.0);
        if (amt_sats < (int64_t)g_pos_min_stake) {
            setStatus(tr("Minimum stake on this network is %1 %2 - a smaller stake would never count.")
                          .arg(QString::number((double)g_pos_min_stake / 100000000.0, 'f', 0), BitcoinUnits::policyAssetTicker()), true);
            return;
        }
    }
    bool ok; QString err;

    // 1) a fresh address whose key we'll stake with
    UniValue addrv = callRpc("getnewaddress", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setStatus(tr("Could not create a staking address: %1").arg(err), true); return; }
    const QString addr = QString::fromStdString(addrv.getValStr());

    // 2) its public key
    UniValue aiparams(UniValue::VARR); aiparams.push_back(addr.toStdString());
    UniValue info = callRpc("getaddressinfo", aiparams, ok, err);
    if (!ok || !info.exists("pubkey")) { setStatus(tr("Could not read the staking key: %1").arg(err), true); return; }
    const QString pubkey = QString::fromStdString(info["pubkey"].get_str());

    // 3) register the stake (funds the staking output)
    UniValue rsparams(UniValue::VARR);
    rsparams.push_back(pubkey.toStdString());
    rsparams.push_back(UniValue(UniValue::VNUM, amount.toStdString()));
    UniValue res = callRpc("registerstake", rsparams, ok, err);
    if (!ok) { setStatus(tr("Staking failed: %1").arg(err), true); return; }
    const QString txid = res.exists("txid") ? QString::fromStdString(res["txid"].getValStr()) : QString();
    const qint64 unbond = res.exists("unbonding_seconds") ? (qint64)res["unbonding_seconds"].get_int64() : 0;

    // 4) the WIF, used to enable block production seamlessly (best-effort export;
    //    legacy wallets only, as with dumpprivkey)
    QString wif;
    UniValue dpparams(UniValue::VARR); dpparams.push_back(addr.toStdString());
    bool dok; QString derr;
    UniValue wifv = callRpc("dumpprivkey", dpparams, dok, derr);
    if (dok) wif = QString::fromStdString(wifv.getValStr());

    QString msg = tr("Staked %1 %4.\nRegistration txid: %2\nStaking public key: %3").arg(amount, txid, pubkey, BitcoinUnits::policyAssetTicker());
    if (unbond > 0) {
        msg += tr("\nUnbonding lock: ~%1 day(s) before you could withdraw (the stake keeps counting the whole time).")
                   .arg(QString::number((double)unbond / 86400.0, 'f', 1));
    }
    // Turn on block production right now — no restart, no manual config. The choice is
    // persisted so it resumes automatically after a restart.
    bool enabled = false; QString enErr;
    if (!wif.isEmpty()) enabled = enableProduction(QStringList{wif}, enErr);
    if (enabled) {
        msg += tr("\n\nBlock production is now ON for this key, automatically, no restart. You'll start "
                  "producing as soon as the stake confirms and the committee elects you, and it resumes "
                  "by itself after a restart.");
    } else if (!wif.isEmpty()) {
        msg += tr("\n\nYour stake is registered, but block production couldn't be turned on automatically "
                  "(%1). Click \"Start producing blocks\" to retry.").arg(enErr);
    } else {
        msg += tr("\n\nYour stake is registered. This wallet can't export the staking key automatically, so "
                  "click \"Start producing blocks\" once the stake confirms to begin producing.");
    }
    m_result->setText(msg);
    setStatus(enabled ? tr("Staked. Block production is on. The stake counts once the transaction confirms.")
                      : tr("Stake registered. It will count once the transaction confirms."), false);
    refresh();
}

bool StakingPage::enableProduction(const QStringList& wifs, QString& err)
{
    if (wifs.isEmpty()) { err = tr("no staking key available to enable"); return false; }
    UniValue arr(UniValue::VARR);
    for (const QString& w : wifs) arr.push_back(w.toStdString());
    UniValue params(UniValue::VARR); params.push_back(arr);
    bool ok;
    UniValue r = callRpc("startposproducer", params, ok, err, /*wallet=*/false);
    return ok && r.isObject() && r.exists("producing") && r["producing"].get_bool();
}

QStringList StakingPage::walletStakingWifs()
{
    QStringList wifs;
    if (!m_wallet_model) return wifs;
    bool ok; QString err;
    UniValue reg = callRpc("getstakerinfo", UniValue(UniValue::VARR), ok, err, /*wallet=*/false);
    if (!ok || !reg.isObject()) return wifs;
    // For each registered staker pubkey, derive an address, check this wallet controls
    // it, and export its WIF. dumpprivkey is best-effort (legacy wallets only).
    for (const std::string& pk : reg.getKeys()) {
        UniValue diParams(UniValue::VARR); diParams.push_back("wpkh(" + pk + ")");
        UniValue di = callRpc("getdescriptorinfo", diParams, ok, err, /*wallet=*/false);
        if (!ok || !di.exists("descriptor")) continue;
        UniValue daParams(UniValue::VARR); daParams.push_back(di["descriptor"].get_str());
        UniValue da = callRpc("deriveaddresses", daParams, ok, err, /*wallet=*/false);
        if (!ok || !da.isArray() || da.empty()) continue;
        const std::string addr = da[0].getValStr();
        UniValue aiParams(UniValue::VARR); aiParams.push_back(addr);
        UniValue ai = callRpc("getaddressinfo", aiParams, ok, err);
        if (!ok || !(ai.exists("ismine") && ai["ismine"].get_bool())) continue;
        UniValue dpParams(UniValue::VARR); dpParams.push_back(addr);
        UniValue wv = callRpc("dumpprivkey", dpParams, ok, err);
        if (ok) wifs << QString::fromStdString(wv.getValStr());
    }
    return wifs;
}

void StakingPage::onEnableProduction()
{
    if (!m_wallet_model) return;
    if (m_enable_button) m_enable_button->setEnabled(false);
    const QStringList wifs = walletStakingWifs();
    if (wifs.isEmpty()) {
        setStatus(tr("No staking keys controlled by this wallet were found. Stake first, then try again."), true);
        if (m_enable_button) m_enable_button->setEnabled(true);
        return;
    }
    QString err;
    const bool enabled = enableProduction(wifs, err);
    setStatus(enabled ? tr("Block production is on for %1 key(s). It resumes automatically after a restart.").arg(wifs.size())
                      : tr("Could not start block production: %1").arg(err), !enabled);
    if (m_enable_button) m_enable_button->setEnabled(true);
    refresh();
}
