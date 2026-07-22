// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <asset.h>
#include <assetsdir.h>
#include <rpc/util.h>

#include <interfaces/node.h>
#include <key.h>
#include <key_io.h>
#include <pos.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// Sequentia theme colours (see qt/res/css/sequentia.css).
const QColor kAccent(0xf5, 0xb3, 0x01);
const QColor kTrack(0x25, 0x25, 0x2c);

//! Format a stake weight (atoms of the policy asset) as whole units, trimmed.
QString FormatWeight(uint64_t atoms)
{
    QString s = QString::number((double)atoms / 100000000.0, 'f', 8);
    if (s.contains('.')) { while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1); }
    return s;
}
} // namespace

StakeShareBar::StakeShareBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(8);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void StakeShareBar::setShare(double share)
{
    m_share = qBound(0.0, share, 1.0);
    update();
}

void StakeShareBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(rect(), 4, 4);
    if (m_share <= 0.0) return;
    // A share this small still deserves a visible sliver: rounding it away would
    // read as "no stake at all".
    const int w = qMax(2, (int)(width() * m_share));
    p.setBrush(kAccent);
    p.drawRoundedRect(QRect(0, 0, w, height()), 4, 4);
}

BlockStripe::BlockStripe(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(10);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BlockStripe::setBlocks(const std::vector<bool>& mine)
{
    m_mine = mine;
    update();
}

void BlockStripe::paintEvent(QPaintEvent*)
{
    if (m_mine.empty()) return;
    QPainter p(this);
    p.setPen(Qt::NoPen);
    const int n = (int)m_mine.size();
    const double step = (double)width() / n;
    for (int i = 0; i < n; ++i) {
        const int x = (int)(i * step);
        const int w = qMax(1, (int)(step) - 1);
        p.setBrush(m_mine[i] ? kAccent : kTrack);
        p.drawRect(QRect(x, 0, w, height()));
    }
}

StakingPage::StakingPage(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent), m_platform_style(platformStyle)
{
    // The page is a tall stack of cards; host it in a scroll area so a larger
    // font (or a shrunk window) can never clip the lower sections, and so this
    // page imposes only a small minimum height on the main window instead of
    // its full content height.
    QVBoxLayout* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    outer->addWidget(scroll);
    QWidget* content = new QWidget(scroll);
    scroll->setWidget(content);

    QVBoxLayout* layout = new QVBoxLayout(content);

    QLabel* title = new QLabel(tr("Staking"), content);
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
    m_enable_button->setToolTip(tr("Turns block production on right now for the staking keys this wallet controls - "
                                   "no config editing, no restart. From then on the node produces a block whenever "
                                   "the committee elects it, collecting the fees, and resumes by itself after a restart."));
    m_enable_button->setVisible(false);
    {
        QHBoxLayout* enRow = new QHBoxLayout();
        enRow->addWidget(m_enable_button);
        enRow->addStretch();
        layout->addLayout(enRow);
    }
    connect(m_enable_button, &QPushButton::clicked, this, &StakingPage::onEnableProduction);

    // --- Your stake: what you hold, how it compares, when you may produce next ---
    {
        QGroupBox* mine = new QGroupBox(tr("Your stake"), this);
        QVBoxLayout* v = new QVBoxLayout(mine);
        QFormLayout* f = new QFormLayout();
        m_my_stake = new QLabel(tr("…"), mine);
        m_my_share = new QLabel(tr("…"), mine);
        f->addRow(tr("Registered stake:"), m_my_stake);
        f->addRow(tr("Share of network stake:"), m_my_share);
        v->addLayout(f);
        m_share_bar = new StakeShareBar(mine);
        m_share_bar->setToolTip(tr("Your slice of all the stake registered on the network. Over time this is "
                                   "roughly the share of blocks — and of the fees — you can expect to collect."));
        v->addWidget(m_share_bar);

        m_next_slot = new QLabel(tr("…"), mine);
        m_next_slot->setWordWrap(true);
        // The draw gates when you may PROPOSE; the committee then backs the lowest
        // draw among the proposals it collected. Saying "slot 0 produces" would
        // promise a block that neither the cadence floor nor the vote guarantees.
        m_next_slot->setToolTip(tr("From the previous block and its Bitcoin anchor every staker derives the same "
                                   "seed, then draws from it with their own secret key — so your draw is yours "
                                   "alone to know, and nobody can tell in advance who comes next. The draw sets the "
                                   "earliest you may offer a block: slot 0 at the usual %1 s cadence, each further "
                                   "slot %1 s later. Whoever is due offers a block, the committee gathers the offers "
                                   "for a few seconds, and then everyone signs the one whose draw came out lowest. "
                                   "So a low slot gets you into that gathering; the draw itself decides who wins it. "
                                   "You find out you led when your block comes back signed by the committee.")
                                    .arg(g_pos_slot_interval));
        QFormLayout* f2 = new QFormLayout();
        f2->addRow(tr("Your draw for the next block:"), m_next_slot);
        v->addLayout(f2);
        layout->addWidget(mine);
    }

    // --- Block production over the recent chain ---
    {
        QGroupBox* prod = new QGroupBox(tr("Block production"), this);
        QVBoxLayout* v = new QVBoxLayout(prod);
        m_produced_count = new QLabel(tr("…"), prod);
        m_produced_count->setWordWrap(true);
        v->addWidget(m_produced_count);
        m_stripe = new BlockStripe(prod);
        m_stripe->setToolTip(tr("One tick per recent block, oldest on the left. The highlighted ones are yours."));
        v->addWidget(m_stripe);
        m_produced_fees = new QLabel(tr("…"), prod);
        m_produced_fees->setWordWrap(true);
        m_produced_fees->setToolTip(tr("Sequentia pays no block subsidy: a producer earns exactly the fees of the "
                                       "blocks it produces, in whichever assets those fees were paid."));
        v->addWidget(m_produced_fees);
        m_last_produced = new QLabel(tr("…"), prod);
        v->addWidget(m_last_produced);
        layout->addWidget(prod);
    }

    // --- Stake action ---
    QGroupBox* stakeGroup = new QGroupBox(tr("Stake %1").arg(BitcoinUnits::policyAssetTicker()), this);
    QFormLayout* stakeForm = new QFormLayout(stakeGroup);
    m_stake_amount = new QLineEdit(stakeGroup);
    m_stake_amount->setPlaceholderText(tr("amount of %1 (at or above the chain minimum, e.g. 50000)").arg(BitcoinUnits::policyAssetTicker()));
    m_stake_amount->setToolTip(tr("What happens when you stake: this amount moves into a staking output that stays "
                                  "yours. It starts counting as soon as the transaction confirms, block production "
                                  "turns on automatically, and every block you produce pays you its fees. If you "
                                  "later withdraw, the only cost is waiting out the unbonding period."));
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

    // --- Unstake action ---
    QGroupBox* unstakeGroup = new QGroupBox(tr("Withdraw stake"), this);
    QFormLayout* unstakeForm = new QFormLayout(unstakeGroup);
    m_unstake_info = new QLabel(tr("…"), unstakeGroup);
    m_unstake_info->setWordWrap(true);
    m_unstake_info->setToolTip(tr("The unbonding wait counts from the moment you staked, not from when you click "
                                  "Withdraw: a stake older than the unbonding period can be withdrawn right away, "
                                  "a younger one tells you here when it unlocks."));
    m_unstake_amount = new QLineEdit(unstakeGroup);
    m_unstake_amount->setPlaceholderText(tr("amount of %1 (leave empty to withdraw everything available)").arg(BitcoinUnits::policyAssetTicker()));
    m_unstake_amount->setToolTip(tr("What happens when you withdraw: the %1 comes back to this wallet as a normal "
                                    "incoming payment, spendable as soon as the withdrawal confirms, and your stake "
                                    "(and share of the fees) shrinks by the withdrawn amount. The network fee is "
                                    "paid out of the withdrawn amount.").arg(BitcoinUnits::policyAssetTicker()));
    {
        QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
        auto* v = new QDoubleValidator(0, 1e15, 8, m_unstake_amount);
        v->setLocale(lc);
        m_unstake_amount->setValidator(v);
    }
    m_unstake_button = new QPushButton(tr("Withdraw"), unstakeGroup);
    m_unstake_result = new QLabel(unstakeGroup);
    m_unstake_result->setWordWrap(true);
    m_unstake_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unstakeForm->addRow(QString(), m_unstake_info);
    unstakeForm->addRow(tr("Amount:"), m_unstake_amount);
    unstakeForm->addRow(QString(), m_unstake_button);
    unstakeForm->addRow(tr("Result:"), m_unstake_result);
    layout->addWidget(unstakeGroup);

    // --- Committee / registry status ---
    QGroupBox* statusGroup = new QGroupBox(tr("Stake registry"), this);
    statusGroup->setToolTip(tr("Everyone staking on the network right now, and with how much weight. Your share of "
                               "the total weight is, on average, the share of blocks (and fees) you produce."));
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

    // --- Watch-only key: follow the staking wallet from anywhere, spend from nowhere ---
    {
        QGroupBox* wo = new QGroupBox(tr("Watch-only key"), this);
        QVBoxLayout* v = new QVBoxLayout(wo);
        QLabel* hint = new QLabel(tr("Master public key of this wallet. Import it into any watch-only wallet to follow "
                                     "your funds and the fees you collect from your phone or another computer. It "
                                     "cannot spend anything."), wo);
        hint->setWordWrap(true);
        v->addWidget(hint);
        QHBoxLayout* row = new QHBoxLayout();
        m_xpub = new QLineEdit(wo);
        m_xpub->setReadOnly(true);
        m_xpub->setPlaceholderText(tr("not available for this wallet type"));
        row->addWidget(m_xpub);
        m_xpub_copy = new QPushButton(tr("Copy"), wo);
        row->addWidget(m_xpub_copy);
        v->addLayout(row);
        // Shown only when there is no key to show, to explain why rather than
        // leave a blank field that looks broken.
        m_xpub_hint = new QLabel(wo);
        m_xpub_hint->setWordWrap(true);
        m_xpub_hint->setVisible(false);
        v->addWidget(m_xpub_hint);
        layout->addWidget(wo);
        connect(m_xpub_copy, &QPushButton::clicked, this, [this] {
            if (!m_xpub || m_xpub->text().isEmpty()) return;
            QApplication::clipboard()->setText(m_xpub->text());
            m_xpub_copy->setText(tr("Copied"));
            QTimer::singleShot(1500, this, [this] { if (m_xpub_copy) m_xpub_copy->setText(tr("Copy")); });
        });
    }

    // --- The blocks this node produced ---
    {
        QGroupBox* blocks = new QGroupBox(tr("Blocks produced by this node"), this);
        QVBoxLayout* v = new QVBoxLayout(blocks);
        m_blocks_summary = new QLabel(tr("…"), blocks);
        m_blocks_summary->setWordWrap(true);
        v->addWidget(m_blocks_summary);
        m_blocks = new QTableWidget(0, 5, blocks);
        m_blocks->setHorizontalHeaderLabels({tr("Height"), tr("Time"), tr("Wait"), tr("Transactions"), tr("Fees collected")});
        m_blocks->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_blocks->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_blocks->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_blocks->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_blocks->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        m_blocks->verticalHeader()->setVisible(false);
        m_blocks->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_blocks->setSelectionBehavior(QAbstractItemView::SelectRows);
        // "Wait" is a fact of the block (its time minus its parent's), not the slot
        // the producer drew: the drawn slot depended on the stake registry as it
        // stood back then, which the node no longer has.
        m_blocks->horizontalHeaderItem(2)->setToolTip(tr("How long after the previous block this one landed. A short "
                                                         "wait means your draw came up early for that block."));
        v->addWidget(m_blocks);
        layout->addWidget(blocks);
    }

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch();

    connect(m_stake_button, &QPushButton::clicked, this, &StakingPage::onStake);
    connect(m_unstake_button, &QPushButton::clicked, this, &StakingPage::onUnstake);
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
    m_status->setStyleSheet(error ? "color:#ff6b6b;" : "color:#3ecf7a;");
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

    refreshOwnStake(reg);
    refreshProducedBlocks();
    refreshWatchOnlyKey();
    refreshUnstakeInfo();

    // Stamp the throttle state so scheduleRefresh() can skip a redundant re-run
    // (same tip, refreshed a moment ago) next time the tab is shown.
    if (m_wallet_model) {
        m_last_refresh_blocks = m_wallet_model->node().getNumBlocks();
        m_last_refresh_ms = QDateTime::currentMSecsSinceEpoch();
    }
}

void StakingPage::scheduleRefresh(bool force)
{
    // A missing wallet model is not a reason to skip: refresh() still updates the
    // block-production banner, which it reads from this node's own config. Only
    // the throttle below needs the model, to ask the node for the tip.
    if (!force && m_wallet_model && m_last_refresh_blocks >= 0) {
        // Nothing new to show since the last refresh: same tip and it ran within
        // the last couple of seconds. The cards/tables already hold that result,
        // so a re-run would only freeze the GUI thread for no visible change.
        const int blocks = m_wallet_model->node().getNumBlocks();
        const qint64 age = QDateTime::currentMSecsSinceEpoch() - m_last_refresh_ms;
        if (blocks == m_last_refresh_blocks && age < 2000) return;
    }
    if (m_refresh_pending) return; // one deferred refresh is enough
    m_refresh_pending = true;
    QPointer<StakingPage> self(this);
    // Let the switch paint first, then run the registry/chain RPCs on the next turn.
    QTimer::singleShot(0, this, [self] {
        if (!self) return;
        self->m_refresh_pending = false;
        self->refresh();
    });
}

void StakingPage::refreshOwnStake(const UniValue& registry)
{
    // Which stakes are ours: the registered keys this wallet controls. Also the
    // key set that tells "was this block ours" further down.
    m_my_pubkeys.clear();
    uint64_t mine = 0, total = 0;
    if (registry.isObject()) {
        for (const std::string& pk : registry.getKeys()) {
            const uint64_t w = registry[pk].isNum() ? (uint64_t)registry[pk].get_int64() : 0;
            total += w;
            // Does this wallet control pk? The answer never changes for a given
            // key, so derive it once (3 RPCs) and cache it — this loop is the
            // heaviest part of a refresh, and re-deriving it on every tab visit
            // is what made the Staking tab crawl.
            auto cached = m_ismine_cache.find(pk);
            bool ismine;
            if (cached != m_ismine_cache.end()) {
                ismine = cached->second;
            } else {
                ismine = false;
                bool ok; QString err;
                // wpkh(<pubkey>) -> address -> does this wallet own it?
                UniValue di(UniValue::VARR); di.push_back("wpkh(" + pk + ")");
                UniValue info = callRpc("getdescriptorinfo", di, ok, err, /*wallet=*/false);
                if (ok && info.exists("descriptor")) {
                    UniValue da(UniValue::VARR); da.push_back(info["descriptor"].get_str());
                    UniValue addrs = callRpc("deriveaddresses", da, ok, err, /*wallet=*/false);
                    if (ok && addrs.isArray() && !addrs.empty()) {
                        UniValue ai(UniValue::VARR); ai.push_back(addrs[0].getValStr());
                        UniValue ainfo = callRpc("getaddressinfo", ai, ok, err);
                        ismine = ok && ainfo.exists("ismine") && ainfo["ismine"].get_bool();
                        // Only cache a definitive answer: on an RPC error, leave it
                        // uncached so a later refresh can still classify the key.
                        if (ok) m_ismine_cache[pk] = ismine;
                    }
                }
            }
            if (!ismine) continue;
            m_my_pubkeys.insert(pk);
            mine += w;
        }
    }
    const double share = total > 0 ? (double)mine / (double)total : 0.0;
    if (m_my_stake) {
        m_my_stake->setText(mine > 0
            ? tr("%1 %2").arg(FormatWeight(mine), BitcoinUnits::policyAssetTicker())
            : tr("none yet — stake below to start producing"));
    }
    if (m_my_share) {
        m_my_share->setText(total > 0
            ? tr("%1% of %2 %3 staked by %4 staker(s)")
                  .arg(QString::number(share * 100.0, 'f', share < 0.01 ? 3 : 1),
                       FormatWeight(total), BitcoinUnits::policyAssetTicker())
                  .arg(registry.isObject() ? (int)registry.getKeys().size() : 0)
            : tr("nothing is staked on the network yet"));
    }
    if (m_share_bar) m_share_bar->setShare(share);

    // Our own draw for the next block. Only the running producer can answer this:
    // the draw needs the staking secret key (see getposslot).
    if (m_next_slot) {
        bool ok; QString err;
        UniValue slot = callRpc("getposslot", UniValue(UniValue::VARR), ok, err, /*wallet=*/false);
        if (!ok || !slot.isObject()) {
            m_next_slot->setText(tr("unavailable: %1").arg(err));
        } else if (!(slot.exists("producing") && slot["producing"].get_bool())) {
            m_next_slot->setText(tr("not producing — no draw is made for this node"));
        } else if (!slot.exists("best_slot") || slot["best_slot"].get_int64() < 0) {
            m_next_slot->setText(tr("no eligible stake — your keys are not in this block's draw"));
        } else {
            const int64_t s = slot["best_slot"].get_int64();
            const int height = slot.exists("height") ? slot["height"].get_int() : 0;
            const int64_t at = slot.exists("best_propose_at") ? slot["best_propose_at"].get_int64() : 0;
            const int64_t wait = at - QDateTime::currentSecsSinceEpoch();
            // Offering the block is what the draw earns you; whether it is the one
            // the committee signs depends on the other offers, which nobody can see
            // in advance. The text must not promise the block.
            m_next_slot->setText(wait > 0
                ? tr("slot %1 — you offer block %2 in about %3 s; it stands if no lower draw offers too")
                      .arg(s).arg(height).arg(wait)
                : tr("slot %1 — you offer block %2 now; it stands if no lower draw offers too")
                      .arg(s).arg(height));
        }
    }
}

void StakingPage::refreshProducedBlocks()
{
    if (!m_blocks) return;
    bool ok; QString err;
    UniValue p(UniValue::VARR);
    p.push_back(100);
    UniValue res = callRpc("getposrecentblocks", p, ok, err, /*wallet=*/false);
    if (!ok || !res.isObject() || !res["blocks"].isArray()) {
        if (m_blocks_summary) m_blocks_summary->setText(tr("Recent blocks unavailable: %1").arg(err));
        return;
    }
    const UniValue& blocks = res["blocks"];

    // The RPC returns newest first; the stripe reads oldest-left.
    std::vector<bool> stripe;
    stripe.reserve(blocks.size());
    for (int i = (int)blocks.size() - 1; i >= 0; --i) {
        const std::string prod = blocks[i]["producer"].getValStr();
        stripe.push_back(m_my_pubkeys.count(prod) > 0);
    }
    if (m_stripe) m_stripe->setBlocks(stripe);

    m_blocks->setRowCount(0);
    CAmountMap fees_total;
    int produced = 0;
    int last_height = -1;
    int64_t last_time = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const UniValue& b = blocks[i];
        if (!m_my_pubkeys.count(b["producer"].getValStr())) continue;
        ++produced;
        if (last_height < 0) { last_height = b["height"].get_int(); last_time = b["time"].get_int64(); }

        // Fees are keyed by asset id; sum them for the card above and render one
        // "<amount> <TICKER>" per asset on the row.
        QStringList parts;
        if (b["fees"].isObject()) {
            for (const std::string& asset_hex : b["fees"].getKeys()) {
                const CAsset asset = GetAssetFromString(asset_hex);
                if (asset.IsNull()) continue;
                const CAmount amt = AmountFromValue(b["fees"][asset_hex]);
                fees_total[asset] += amt;
                parts << GUIUtil::formatAssetAmount(asset, amt, BitcoinUnits::BTC,
                                                    BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true);
            }
        }
        const int row = m_blocks->rowCount();
        m_blocks->insertRow(row);
        m_blocks->setItem(row, 0, new QTableWidgetItem(QString::number(b["height"].get_int())));
        m_blocks->setItem(row, 1, new QTableWidgetItem(
            QDateTime::fromSecsSinceEpoch(b["time"].get_int64()).toString("yyyy-MM-dd HH:mm")));
        m_blocks->setItem(row, 2, new QTableWidgetItem(tr("%1 s").arg(b["wait"].get_int64())));
        m_blocks->setItem(row, 3, new QTableWidgetItem(QString::number(b["txs"].get_int())));
        m_blocks->setItem(row, 4, new QTableWidgetItem(parts.isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                                                       : parts.join(" + ")));
    }

    const int scanned = res.exists("scanned") ? res["scanned"].get_int() : 0;
    if (m_produced_count) {
        m_produced_count->setText(produced > 0
            ? tr("%1 of the last %2 blocks were produced by this node.").arg(produced).arg(scanned)
            : tr("None of the last %1 blocks were produced by this node.").arg(scanned));
    }
    if (m_produced_fees) {
        const QString sum = GUIUtil::formatMultiAssetAmountWithValue(
            fees_total, BitcoinUnits::BTC, BitcoinUnits::SeparatorStyle::STANDARD,
            m_wallet_model ? m_wallet_model->getOptionsModel()->getReferenceCurrency() : QString(), ", ");
        m_produced_fees->setText(produced > 0 ? tr("Fees collected over them: %1").arg(sum)
                                              : tr("Fees collected over them: none"));
    }
    if (m_last_produced) {
        m_last_produced->setText(last_height >= 0
            ? tr("Last block produced: %1, at %2").arg(last_height)
                  .arg(QDateTime::fromSecsSinceEpoch(last_time).toString("yyyy-MM-dd HH:mm"))
            : QString());
        m_last_produced->setVisible(last_height >= 0);
    }
    if (m_blocks_summary) {
        m_blocks_summary->setText(produced > 0
            ? tr("Every block below paid you its fees. Blocks scanned: %1 (heights %2–%3).")
                  .arg(scanned).arg(res["from_height"].get_int()).arg(res["to_height"].get_int())
            : tr("Nothing yet in the last %1 blocks. With a small share of the stake this is normal — "
                 "your draw comes up less often.").arg(scanned));
    }
}

void StakingPage::refreshWatchOnlyKey()
{
    if (!m_xpub || !m_xpub->text().isEmpty()) return; // fetched once; it does not change
    bool ok; QString err;
    UniValue descs = callRpc("listdescriptors", UniValue(UniValue::VARR), ok, err);
    if (ok && descs.isObject() && descs["descriptors"].isArray()) {
        // The receiving descriptor carries the wallet's master public key; hand the
        // whole descriptor over, since that is what a watch-only wallet imports.
        const UniValue& arr = descs["descriptors"];
        for (size_t i = 0; i < arr.size(); ++i) {
            const UniValue& d = arr[i];
            if (d.exists("internal") && d["internal"].get_bool()) continue;
            if (!d.exists("desc")) continue;
            m_xpub->setText(QString::fromStdString(d["desc"].get_str()));
            m_xpub->setCursorPosition(0);
            if (m_xpub_hint) m_xpub_hint->setVisible(false);
            return;
        }
    }
    // listdescriptors failed or returned nothing. The usual reason is a legacy
    // (pre-descriptor) wallet, which has no single master public key to export;
    // say so, and what to do about it, rather than leave a blank field that reads
    // as a bug. A locked wallet reports the same, so mention that too.
    if (!m_xpub_hint) return;
    m_xpub->setPlaceholderText(tr("not available for this wallet"));
    m_xpub_hint->setText(tr("This wallet is a legacy (non-descriptor) wallet, which has no single master "
                            "public key to hand out. To follow it watch-only, create a new wallet with "
                            "\"Descriptor wallet\" ticked and move your stake to it, or export individual "
                            "addresses instead. (If the wallet is simply locked, unlock it and press Refresh.)"));
    m_xpub_hint->setVisible(true);
}

void StakingPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // Never refresh synchronously here: the registry/chain RPC cascade on the GUI
    // thread would block the new tab from painting and make the switch feel like a
    // stall. Defer it, and skip it entirely when nothing changed since last time.
    scheduleRefresh(/*force=*/false);
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
    // An explicit refresh means "recompute everything from scratch": drop the
    // per-staker ownership cache so ismine is re-derived for every registered key.
    m_ismine_cache.clear();
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

void StakingPage::refreshUnstakeInfo()
{
    if (!m_unstake_info || !m_wallet_model) return;
    bool ok; QString err;
    UniValue list = callRpc("liststakeutxos", UniValue(UniValue::VARR), ok, err);
    if (!ok || !list.isArray()) {
        m_unstake_info->setText(tr("Staked coins unavailable: %1").arg(err));
        if (m_unstake_button) m_unstake_button->setEnabled(false);
        return;
    }
    CAmount mature = 0, immature = 0;
    QString next_unlock;
    int64_t best_height = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const UniValue& o = list[i];
        CAmount amt = 0;
        try { amt = AmountFromValue(o["amount"]); } catch (...) { continue; }
        if (o["withdrawable"].isBool() && o["withdrawable"].get_bool()) {
            mature += amt;
            continue;
        }
        immature += amt;
        // Remember the stake that unlocks first; its status says when.
        const int64_t h = o["spendable_height"].isNum() ? o["spendable_height"].get_int64()
                                                        : std::numeric_limits<int64_t>::max();
        if (best_height < 0 || h < best_height) {
            best_height = h;
            next_unlock = QString::fromStdString(o["status"].getValStr());
        }
    }
    const QString ticker = BitcoinUnits::policyAssetTicker();
    QString text;
    if (mature == 0 && immature == 0) {
        text = tr("Nothing is staked from this wallet yet.");
    } else if (immature == 0) {
        text = tr("Withdrawable now: %1 %2. The unbonding wait for these coins has already been served.")
                   .arg(FormatWeight((uint64_t)mature), ticker);
    } else if (mature == 0) {
        text = tr("Still unbonding: %1 %2 — %3. Nothing can be withdrawn before then; the stake keeps "
                  "counting (and earning) the whole time.")
                   .arg(FormatWeight((uint64_t)immature), ticker, next_unlock);
    } else {
        text = tr("Withdrawable now: %1 %3. Still unbonding: %2 %3 (%4).")
                   .arg(FormatWeight((uint64_t)mature), FormatWeight((uint64_t)immature), ticker, next_unlock);
    }
    m_unstake_info->setText(text);
    if (m_unstake_button) m_unstake_button->setEnabled(mature > 0);
}

void StakingPage::onUnstake()
{
    if (!m_wallet_model) return;
    bool ok; QString err;

    // What the wallet has staked, and how much of it is withdrawable right now.
    UniValue list = callRpc("liststakeutxos", UniValue(UniValue::VARR), ok, err);
    if (!ok || !list.isArray()) { setStatus(tr("Could not read the staked coins: %1").arg(err), true); return; }
    CAmount mature_total = 0, immature_total = 0;
    std::set<std::string> mature_keys;
    QString next_unlock;
    int64_t best_height = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const UniValue& o = list[i];
        CAmount amt = 0;
        try { amt = AmountFromValue(o["amount"]); } catch (...) { continue; }
        if (o["withdrawable"].isBool() && o["withdrawable"].get_bool()) {
            mature_total += amt;
            mature_keys.insert(o["pubkey"].getValStr());
            continue;
        }
        immature_total += amt;
        const int64_t h = o["spendable_height"].isNum() ? o["spendable_height"].get_int64()
                                                        : std::numeric_limits<int64_t>::max();
        if (best_height < 0 || h < best_height) {
            best_height = h;
            next_unlock = QString::fromStdString(o["status"].getValStr());
        }
    }
    const QString ticker = BitcoinUnits::policyAssetTicker();
    if (mature_total == 0 && immature_total == 0) {
        setStatus(tr("Nothing is staked from this wallet."), true);
        return;
    }
    if (mature_total == 0) {
        setStatus(tr("Nothing can be withdrawn yet: %1.").arg(next_unlock), true);
        return;
    }

    // The amount. Empty means everything that is withdrawable.
    const QString amount_text = m_unstake_amount ? m_unstake_amount->text().trimmed() : QString();
    CAmount want = mature_total;
    bool partial = false;
    if (!amount_text.isEmpty()) {
        bool amtok = false;
        const double amtval = amount_text.toDouble(&amtok);
        if (!amtok || amtval <= 0) {
            setStatus(tr("Enter a positive %1 amount, or leave the field empty to withdraw everything available.").arg(ticker), true);
            return;
        }
        want = (CAmount)std::llround(amtval * 100000000.0);
        if (want > mature_total) {
            setStatus(tr("Only %1 %2 can be withdrawn right now%3").arg(
                          FormatWeight((uint64_t)mature_total), ticker,
                          immature_total > 0 ? tr(" — the rest is still unbonding.") : QString(".")), true);
            return;
        }
        partial = want < mature_total;
    }
    if (partial && mature_keys.size() > 1) {
        setStatus(tr("This wallet stakes with more than one key, so a partial withdrawal is ambiguous here. "
                     "Withdraw everything, or use the withdrawstake RPC to pick a key."), true);
        return;
    }

    // Numbers for the confirmation: our registered stake and the network total.
    // Weights and coin amounts share the same unit (1e-8), so they compare directly.
    const CAmount my_total = mature_total + immature_total;
    double net_total = 0;
    UniValue reg = callRpc("getstakerinfo", UniValue(UniValue::VARR), ok, err, /*wallet=*/false);
    if (ok && reg.isObject()) {
        for (const std::string& pk : reg.getKeys()) {
            if (reg[pk].isNum()) net_total += (double)reg[pk].get_int64();
        }
    }
    const double before = net_total > 0 ? (double)my_total / net_total : 0.0;
    const double after_den = net_total - (double)want;
    const double after = after_den > 0 ? (double)(my_total - want) / after_den : 0.0;

    QString msg = tr("You are about to withdraw %1 %2 from your stake.")
                      .arg(FormatWeight((uint64_t)want), ticker);
    msg += "\n\n";
    msg += tr("The %1 returns to this wallet at a fresh receiving address, as a normal incoming payment, "
              "minus the network fee. It is spendable as soon as the withdrawal confirms: the unbonding "
              "wait started when you staked these coins, and it has already been served.")
               .arg(ticker);
    msg += "\n\n";
    msg += tr("When the withdrawal confirms, your registered stake drops from %1 to %2 %3")
               .arg(FormatWeight((uint64_t)my_total), FormatWeight((uint64_t)(my_total - want)), ticker);
    if (net_total > 0) {
        msg += tr(", and your share of the network stake goes from %1% to about %2%. You will be elected "
                  "to produce blocks (and collect their fees) correspondingly less often.")
                   .arg(QString::number(before * 100.0, 'f', before < 0.01 ? 3 : 1),
                        QString::number(after * 100.0, 'f', after < 0.01 ? 3 : 1));
    } else {
        msg += tr(".");
    }
    if (partial) {
        msg += "\n\n";
        msg += tr("The rest of your stake keeps staking. If a staked coin has to be split to withdraw this "
                  "exact amount, the remainder is re-staked automatically and its unbonding clock restarts.");
    }
    if (QMessageBox::question(this, tr("Withdraw stake?"), msg,
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    if (m_unstake_button) m_unstake_button->setEnabled(false);
    UniValue params(UniValue::VARR);
    if (partial) {
        // A partial withdrawal names the (single) staker key and the exact
        // amount, as typed — the RPC re-parses it, avoiding any double rounding.
        params.push_back(*mature_keys.begin());
        params.push_back(UniValue(UniValue::VNUM, amount_text.toStdString()));
    }
    UniValue res = callRpc("withdrawstake", params, ok, err);
    if (m_unstake_button) m_unstake_button->setEnabled(true);
    if (!ok) {
        setStatus(tr("The withdrawal failed: %1").arg(err), true);
        return;
    }

    const QString txid = res.exists("txid") ? QString::fromStdString(res["txid"].getValStr()) : QString();
    const QString dest = res.exists("destination") ? QString::fromStdString(res["destination"].getValStr()) : QString();
    const QString amt = res.exists("amount") ? QString::fromStdString(res["amount"].getValStr()) : QString();
    const QString fee = res.exists("fee") ? QString::fromStdString(res["fee"].getValStr()) : QString();
    QString out = tr("Withdrew %1 %2 to %3 (network fee %4 %2).\nTransaction: %5").arg(amt, ticker, dest, fee, txid);
    if (res.exists("restaked")) {
        out += tr("\nRe-staked remainder: %1 %2 (its unbonding clock restarted).")
                   .arg(QString::fromStdString(res["restaked"].getValStr()), ticker);
    }
    if (res.exists("share_before") && res.exists("share_after")) {
        out += tr("\nShare of the network stake once it confirms: %1% → %2%.")
                   .arg(QString::number(res["share_before"].get_real() * 100.0, 'f', 2),
                        QString::number(res["share_after"].get_real() * 100.0, 'f', 2));
    }
    if (m_unstake_result) m_unstake_result->setText(out);
    if (m_unstake_amount) m_unstake_amount->clear();
    setStatus(tr("Withdrawal sent. Your stake updates when the transaction confirms."), false);
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
