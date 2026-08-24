// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakingpage.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QSignalBlocker>

#include <qt/bitcoinunits.h>
#include <qt/collapsiblesection.h>
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
#include <QGuiApplication>
#include <QScreen>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QComboBox>
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

//! Ask a yes/no question in a dialog placed over the window the user is working
//! in. Qt leaves the placement to the window manager, and some of them (WSLg's
//! among them) park it at the top-left corner BEHIND the main window: a
//! confirmation the user never sees is a confirmation that did not happen, and
//! here it guards moving real funds.
int AskCentred(QWidget* parent, const QString& title, const QString& text)
{
    QWidget* top = parent ? parent->window() : nullptr;
    QMessageBox box(QMessageBox::Question, title, text,
                    QMessageBox::Yes | QMessageBox::Cancel, top);
    box.setDefaultButton(QMessageBox::Cancel);
    QPointer<QMessageBox> guard(&box);
    // Only once exec() has laid the dialog out does it have a real size to
    // centre, so do it on the next turn of the event loop.
    QTimer::singleShot(0, &box, [guard, top] {
        if (!guard) return;
        if (top && top->isVisible()) {
            QRect g = guard->frameGeometry();
            g.moveCenter(top->frameGeometry().center());
            // A parent straddling a screen edge must not push the dialog off it.
            if (QScreen* screen = guard->screen() ? guard->screen() : QGuiApplication::primaryScreen()) {
                const QRect avail = screen->availableGeometry();
                if (!avail.isEmpty()) {
                    QPoint tl = g.topLeft();
                    tl.setX(qBound(avail.left(), tl.x(), qMax(avail.left(), avail.right() - g.width() + 1)));
                    tl.setY(qBound(avail.top(), tl.y(), qMax(avail.top(), avail.bottom() - g.height() + 1)));
                    g.moveTopLeft(tl);
                }
            }
            guard->move(g.topLeft());
        }
        guard->raise();
        guard->activateWindow();
    });
    return box.exec();
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
        // Two different numbers, and conflating them misreads the mechanism: the
        // cadence (%1) is how often the chain produces a block at all, while the
        // draw is scaled by the slot interval (%2). Every draw below %1/%2 lands
        // under the cadence, so those stakers all offer at the same moment.
        m_next_slot->setToolTip(tr("From the previous block and its Bitcoin anchor every staker derives the same "
                                   "seed, then draws from it with their own secret key — so your draw is yours "
                                   "alone to know, and nobody can tell in advance who comes next. The chain makes "
                                   "a block every %1 s, and no block may be closer than that to the one before it. "
                                   "Your draw sets the earliest you may offer one: draw 0 straight away, each "
                                   "further draw %2 s later — so every draw that lands inside the %1 s cadence "
                                   "offers at the same moment. Whoever is due offers a block, the committee gathers "
                                   "the offers for a few seconds, and then everyone signs the one whose draw came "
                                   "out lowest. So a low draw gets you into that gathering; the draw itself decides "
                                   "who wins it. You find out you led when your block comes back signed by the "
                                   "committee.")
                                    .arg(Params().GetConsensus().pos_block_spacing > 0
                                             ? Params().GetConsensus().pos_block_spacing
                                             : g_pos_slot_interval)
                                    .arg(g_pos_slot_interval));
        QFormLayout* f2 = new QFormLayout();
        f2->addRow(tr("Your draw for the next block:"), m_next_slot);
        v->addLayout(f2);
        layout->addWidget(mine);
        // The one card that stays open: it is the answer the page exists to
        // give. Every other card here is either an action you have to ask for
        // or a detail behind this one, so they fold to their titles.
        CollapsibleSection::adopt(mine, QStringLiteral("staking/mine"), /*open_by_default=*/true);
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
        m_production_section = CollapsibleSection::adopt(prod, QStringLiteral("staking/production"));
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
    // Staking the whole balance can never work: the transaction still has to pay
    // its own network fee, which comes out of the same coins. Max fills in the
    // balance minus a little headroom for it, so the obvious action succeeds
    // instead of failing with "insufficient funds".
    m_stake_max = new QPushButton(tr("Max"), stakeGroup);
    m_stake_max->setToolTip(tr("Stake as much as possible: your whole %1 balance, less a small amount left over to "
                               "pay the transaction's network fee.").arg(BitcoinUnits::policyAssetTicker()));
    m_result = new QLabel(stakeGroup);
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {
        QWidget* row = new QWidget(stakeGroup);
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(m_stake_amount);
        h->addWidget(m_stake_max);
        stakeForm->addRow(tr("Amount:"), row);
    }
    stakeForm->addRow(QString(), m_stake_button);
    stakeForm->addRow(tr("Result:"), m_result);
    layout->addWidget(stakeGroup);
    CollapsibleSection::adopt(stakeGroup, QStringLiteral("staking/stake"));

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
    m_unstake_max = new QPushButton(tr("Max"), unstakeGroup);
    m_unstake_max->setToolTip(tr("Withdraw everything that is withdrawable right now."));
    unstakeForm->addRow(QString(), m_unstake_info);
    {
        QWidget* row = new QWidget(unstakeGroup);
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(m_unstake_amount);
        h->addWidget(m_unstake_max);
        unstakeForm->addRow(tr("Amount:"), row);
    }
    // The button is disabled while nothing is withdrawable, and Qt does not
    // deliver tooltip events to a disabled widget — so the "why is this grey"
    // explanation has to hang on an enabled container around it.
    m_unstake_bump = new QPushButton(tr("Speed up (higher fee)"), unstakeGroup);
    m_unstake_bump->setToolTip(tr("Re-send the withdrawal that is waiting to confirm, paying a higher network fee "
                                  "so it is picked up sooner. It goes to the same address for the same amount, "
                                  "less the extra fee."));
    m_unstake_bump->setVisible(false);
    m_unstake_button_holder = new QWidget(unstakeGroup);
    {
        QHBoxLayout* h = new QHBoxLayout(m_unstake_button_holder);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(m_unstake_button);
        h->addWidget(m_unstake_bump);
        h->addStretch();
    }
    unstakeForm->addRow(QString(), m_unstake_button_holder);
    unstakeForm->addRow(tr("Result:"), m_unstake_result);
    layout->addWidget(unstakeGroup);
    CollapsibleSection::adopt(unstakeGroup, QStringLiteral("staking/unstake"));

    // --- Staking pool: lend your weight to a pool, or watch the one you are in ---
    //
    // Delegating does not move the staked coins and cannot: the pool's key is
    // nowhere in the staking output's spending condition. What it lends is the
    // right to produce blocks with this stake's weight, in a separate little
    // record that the staker alone can spend -- which is why leaving is instant
    // and needs nobody's cooperation.
    {
        QGroupBox* pool = new QGroupBox(tr("Staking pool"), this);
        QVBoxLayout* v = new QVBoxLayout(pool);

        m_deleg_alerts = new QLabel(pool);
        m_deleg_alerts->setWordWrap(true);
        m_deleg_alerts->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_deleg_alerts->setVisible(false);
        v->addWidget(m_deleg_alerts);

        m_deleg_status = new QLabel(tr("…"), pool);
        m_deleg_status->setWordWrap(true);
        m_deleg_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_deleg_status);

        QLabel* hint = new QLabel(tr("A pool produces blocks on your behalf, so a holding too small to stake on "
                                     "its own, or a wallet you would rather keep closed, still earns. There is no "
                                     "minimum to delegate: the network judges a pool on what it commands in "
                                     "total. Your coins never move and the pool can never spend them — it is only "
                                     "lent the right to sign with your weight, and you can take that back at any "
                                     "moment, without asking.\n\nPools are listed, with what each has committed "
                                     "to paying and how reliably it produces, on the pool board at "
                                     "sequentiatestnet.com/pools/. Paste the signer key of the one you choose."),
                                  pool);
        hint->setWordWrap(true);
        v->addWidget(hint);

        QFormLayout* form = new QFormLayout();
        m_deleg_signer = new QLineEdit(pool);
        m_deleg_signer->setPlaceholderText(tr("pool signer public key (66 hex characters)"));
        m_deleg_signer->setToolTip(tr("Pools publish this key. The public pool board lists every pool with what "
                                      "it has committed to paying and how reliably it produces."));

        m_deleg_amount = new QLineEdit(pool);
        m_deleg_amount->setPlaceholderText(tr("amount of %1 to stake and lend (leave empty to lend what you "
                                              "already stake)").arg(BitcoinUnits::policyAssetTicker()));
        m_deleg_amount->setToolTip(tr("There is NO minimum here. The network judges a pool on the weight it "
                                      "commands in total, not on what each delegator brings, which is exactly "
                                      "why pooling exists. Staking on your own is the path with a minimum."));
        {
            QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
            auto* v = new QDoubleValidator(0, 1e15, 8, m_deleg_amount);
            v->setLocale(lc);
            m_deleg_amount->setValidator(v);
        }
        m_deleg_button = new QPushButton(tr("Delegate to this pool"), pool);
        m_undeleg_button = new QPushButton(tr("Take my stake back"), pool);
        m_undeleg_button->setToolTip(tr("Stop delegating: your stake's weight counts for you again from the next "
                                        "confirmation. This does not unstake — your coins were never moved, and "
                                        "are not moved now."));
        {
            QWidget* row = new QWidget(pool);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(m_deleg_button);
            h->addWidget(m_undeleg_button);
            h->addStretch();
            form->addRow(tr("Pool:"), m_deleg_signer);
            form->addRow(tr("Amount:"), m_deleg_amount);
            form->addRow(QString(), row);
        }
        m_deleg_result = new QLabel(pool);
        m_deleg_result->setWordWrap(true);
        m_deleg_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(tr("Result:"), m_deleg_result);
        v->addLayout(form);
        layout->addWidget(pool);
        m_pool_section = CollapsibleSection::adopt(pool, QStringLiteral("staking/pool"));
    }

    // --- Run a staking pool: the operator console ---
    //
    // This card exists ONLY in the node wallet, on purpose. Running a pool means
    // producing blocks, which means being online and holding the signing key on
    // the machine that produces them; and announcing a payout policy binds every
    // block that key ever produces, in a commitment strangers audit before
    // lending you their weight. A browser or phone wallet can do neither, so
    // offering the button there would promise something it cannot keep.
    {
        QGroupBox* op = new QGroupBox(tr("Run a staking pool"), this);
        QVBoxLayout* v = new QVBoxLayout(op);

        m_pool_status = new QLabel(tr("…"), op);
        m_pool_status->setWordWrap(true);
        m_pool_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_pool_status);

        QLabel* hint = new QLabel(tr("Other stakers can lend you their weight, which makes you produce blocks more "
                                     "often without ever giving you their coins. What they are trusting you for is "
                                     "the reward, so commit to how you will share it. A producer that has committed "
                                     "to nothing keeps everything, which is honest but gives nobody a reason to "
                                     "join you."), op);
        hint->setWordWrap(true);
        v->addWidget(hint);

        m_pool_commitment = new QLabel(op);
        m_pool_commitment->setWordWrap(true);
        m_pool_commitment->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(m_pool_commitment);

        QFormLayout* form = new QFormLayout();
        m_payout_signer = new QComboBox(op);
        m_payout_signer->setToolTip(tr("A payout policy binds ONE block-producing key. This wallet stakes with "
                                       "more than one, so choose which."));
        m_payout_signer_label = new QLabel(tr("Signer:"), op);
        form->addRow(m_payout_signer_label, m_payout_signer);
        m_payout_signer->setVisible(false);
        m_payout_signer_label->setVisible(false);
        // Most operators want one of a handful of ordinary arrangements. Asking
        // them to compose one from a mode and a basis-point figure makes the
        // common case harder than the rare one, and a commission invented on the
        // spot is a commission nobody reasoned about. The data is
        // "<mode>:<commission bp>", or "custom".
        // Five arrangements, each a thing an operator actually wants to say. The
        // data is "<mode>:<commission bp>", "lottery:ask" when the operator sets
        // the percentage, or "custom" for the raw commitment.
        m_payout_preset = new QComboBox(op);
        m_payout_preset->addItem(tr("Share everything: every delegator gets its exact proportional share"),
                                 QStringLiteral("split:0"));
        m_payout_preset->addItem(tr("Share, keeping 5%"), QStringLiteral("split:500"));
        m_payout_preset->addItem(tr("Share, keeping a commission I choose"), QStringLiteral("split:ask"));
        m_payout_preset->addItem(tr("Lottery: each block pays one delegator, drawn by stake weight"),
                                 QStringLiteral("lottery:ask"));
        m_payout_preset->addItem(tr("Pay one address I commit to, every block"), QStringLiteral("direct:0"));
        m_payout_preset->addItem(tr("Custom: commit a script of my own"), QStringLiteral("custom"));
        m_payout_preset->setCurrentIndex(1);   // keeping 5%: the one most operators want
        form->addRow(tr("Payout:"), m_payout_preset);

        m_payout_preset_note = new QLabel(op);
        m_payout_preset_note->setWordWrap(true);
        m_payout_preset_note->setStyleSheet("QLabel{color:#666;}");
        form->addRow(QString(), m_payout_preset_note);

        m_payout_commission = new QLineEdit(op);
        m_payout_commission->setPlaceholderText(tr("percent you keep, e.g. 5 (0 is allowed)"));
        m_payout_commission->setToolTip(tr("The share of blocks you keep. At 0 you still earn on your own stake, as "
                                           "one participant among your delegators."));
        {
            QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
            auto* val = new QDoubleValidator(0, 100, 2, m_payout_commission);
            val->setLocale(lc);
            m_payout_commission->setValidator(val);
        }
        m_payout_commission_label = new QLabel(tr("Commission:"), op);
        form->addRow(m_payout_commission_label, m_payout_commission);

        m_payout_address = new QLineEdit(op);
        m_payout_address->setPlaceholderText(tr("address every block must pay (leave empty for a fresh one of yours)"));
        m_payout_address_label = new QLabel(tr("Pay to:"), op);
        form->addRow(m_payout_address_label, m_payout_address);

        m_payout_script = new QLineEdit(op);
        m_payout_script->setPlaceholderText(tr("scriptPubKey in hex, up to 110 bytes"));
        m_payout_script->setToolTip(tr("The exact bytes every coinbase must pay. Consensus compares the output "
                                       "against these and asks nothing else of them, so anything expressible as "
                                       "a script works: a multisig, a covenant, a contract that splits the "
                                       "reward. It is equally on you that they can be spent at all — a script "
                                       "nobody can satisfy burns every reward this key earns, and the chain will "
                                       "enforce that just as faithfully."));
        m_payout_script_label = new QLabel(tr("Payout script:"), op);
        form->addRow(m_payout_script_label, m_payout_script);

        m_payout_activation = new QLineEdit(op);
        m_payout_activation->setPlaceholderText(tr("block height (leave empty for the earliest allowed)"));
        m_payout_activation->setToolTip(tr("The height the policy binds from. It cannot be inside the chain's "
                                           "notice period, which is what gives your delegators time to read the "
                                           "change and leave."));
        {
            QLocale lc(QLocale::C); lc.setNumberOptions(QLocale::RejectGroupSeparator);
            m_payout_activation->setValidator(new QIntValidator(1, 2000000000, m_payout_activation));
        }
        m_payout_activation_label = new QLabel(tr("Binds at height:"), op);
        form->addRow(m_payout_activation_label, m_payout_activation);

        m_payout_button = new QPushButton(tr("Announce this policy"), op);
        form->addRow(QString(), m_payout_button);
        m_payout_result = new QLabel(op);
        m_payout_result->setWordWrap(true);
        m_payout_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(tr("Result:"), m_payout_result);
        v->addLayout(form);
        layout->addWidget(op);
        CollapsibleSection::adopt(op, QStringLiteral("staking/operator"));
    }

    // --- Staking rewards, and converting them ---
    {
        QGroupBox* rw = new QGroupBox(tr("Staking rewards"), this);
        rw->setToolTip(tr("What staking has paid you, in whatever assets the fees were paid in. Sequentia has no "
                          "block subsidy: a block earns its own transaction fees and nothing else, so rewards "
                          "arrive in the assets the payers chose."));
        QVBoxLayout* v = new QVBoxLayout(rw);

        QLabel* blurb = new QLabel(tr("Block rewards are spendable 100 blocks after they are earned; a pool payout "
                                      "is spendable at once."), rw);
        blurb->setWordWrap(true);
        v->addWidget(blurb);

        m_rewards_table = new QTableWidget(0, 3, rw);
        m_rewards_table->setHorizontalHeaderLabels({tr("Asset"), tr("Spendable"), tr("Maturing")});
        m_rewards_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_rewards_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_rewards_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_rewards_table->verticalHeader()->setVisible(false);
        m_rewards_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        v->addWidget(m_rewards_table);

        m_ac_enabled = new QCheckBox(tr("Convert my staking rewards automatically"), rw);
        m_ac_enabled->setToolTip(tr("Sell each reward for one asset of your choosing on SeqDEX, as the rewards "
                                    "arrive and whenever there is a market for the pair."));
        v->addWidget(m_ac_enabled);

        QLabel* warn = new QLabel(tr("<b>Once this is on, the wallet sells without asking again.</b> It never "
                                     "converts more than staking has paid you, never touches your stake, and never "
                                     "converts what it cannot get a fair price for — but the selling itself is "
                                     "unattended, which is the point of it."), rw);
        warn->setWordWrap(true);
        v->addWidget(warn);

        m_ac_form = new QWidget(rw);
        QFormLayout* f = new QFormLayout(m_ac_form);
        m_ac_target = new QComboBox(m_ac_form);
        m_ac_target->setToolTip(tr("Bitcoin is the parent chain's own coin, delivered to this wallet's Bitcoin "
                                   "address. Any other choice is a Sequentia asset, delivered here."));
        f->addRow(tr("Convert into"), m_ac_target);
        m_ac_min = new QLineEdit(m_ac_form);
        m_ac_min->setPlaceholderText(QStringLiteral("0.0001"));
        m_ac_min->setToolTip(tr("Smaller batches wait for the next reward rather than paying a swap's costs to "
                                "convert dust."));
        f->addRow(tr("Only once worth at least"), m_ac_min);
        m_ac_slippage = new QComboBox(m_ac_form);
        m_ac_slippage->addItem(tr("0.5% off the reference price"), 50);
        m_ac_slippage->addItem(tr("1%"), 100);
        m_ac_slippage->addItem(tr("2%"), 200);
        m_ac_slippage->addItem(tr("5%"), 500);
        f->addRow(tr("Refuse a price worse than"), m_ac_slippage);
        m_ac_save = new QPushButton(tr("Save"), m_ac_form);
        f->addRow(QString(), m_ac_save);
        m_ac_form->setVisible(false);
        v->addWidget(m_ac_form);

        m_ac_status = new QLabel(rw);
        m_ac_status->setWordWrap(true);
        v->addWidget(m_ac_status);

        // Converting into Bitcoin is a swap across two chains, so there is a
        // stretch where the asset is locked in a contract and the Bitcoin has
        // not arrived. Nothing needs doing during it -- the wallet finishes or
        // refunds by itself -- but a staker who cannot SEE it has no way to
        // tell a swap that is merely slow from one that has gone wrong. This
        // appears only while there is something to show.
        m_swaps_box = new QWidget(rw);
        {
            QVBoxLayout* sv = new QVBoxLayout(m_swaps_box);
            sv->setContentsMargins(0, 8, 0, 0);
            m_swaps_intro = new QLabel(m_swaps_box);
            m_swaps_intro->setWordWrap(true);
            sv->addWidget(m_swaps_intro);
            m_swaps_table = new QTableWidget(0, 4, m_swaps_box);
            m_swaps_table->setHorizontalHeaderLabels(
                {tr("Selling"), tr("For"), tr("Stage"), tr("If nothing happens")});
            m_swaps_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            m_swaps_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            m_swaps_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
            m_swaps_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
            m_swaps_table->verticalHeader()->setVisible(false);
            m_swaps_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            sv->addWidget(m_swaps_table);
            m_swaps_resume = new QPushButton(tr("Carry these on now"), m_swaps_box);
            m_swaps_resume->setToolTip(tr("The wallet does this by itself every couple of minutes. This is for "
                                          "when you would rather not wait."));
            QHBoxLayout* srow = new QHBoxLayout();
            srow->addStretch();
            srow->addWidget(m_swaps_resume);
            sv->addLayout(srow);
        }
        m_swaps_box->setVisible(false);
        v->addWidget(m_swaps_box);

        connect(m_ac_enabled, &QCheckBox::toggled, this, &StakingPage::onRewardConvertToggled);
        connect(m_ac_save, &QPushButton::clicked, this, &StakingPage::onRewardConvertSave);
        connect(m_swaps_resume, &QPushButton::clicked, this, &StakingPage::onResumeSwaps);

        layout->addWidget(rw);
        CollapsibleSection::adopt(rw, QStringLiteral("staking/rewards"));
    }

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
    m_registry_section = CollapsibleSection::adopt(statusGroup, QStringLiteral("staking/registry"));

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
        CollapsibleSection::adopt(wo, QStringLiteral("staking/watchonly"));
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
        m_blocks_section = CollapsibleSection::adopt(blocks, QStringLiteral("staking/blocks"));
    }

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch();

    connect(m_stake_button, &QPushButton::clicked, this, &StakingPage::onStake);
    connect(m_stake_max, &QPushButton::clicked, this, &StakingPage::onStakeMax);
    connect(m_unstake_button, &QPushButton::clicked, this, &StakingPage::onUnstake);
    connect(m_unstake_max, &QPushButton::clicked, this, &StakingPage::onUnstakeMax);
    connect(m_unstake_bump, &QPushButton::clicked, this, &StakingPage::onUnstakeBump);
    connect(m_refresh_button, &QPushButton::clicked, this, &StakingPage::onRefreshClicked);
    connect(m_deleg_button, &QPushButton::clicked, this, &StakingPage::onDelegate);
    connect(m_undeleg_button, &QPushButton::clicked, this, &StakingPage::onUndelegate);
    connect(m_payout_button, &QPushButton::clicked, this, &StakingPage::onAnnouncePayout);
    connect(m_payout_preset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &StakingPage::onPayoutPresetChanged);
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

void StakingPage::setCardResult(QLabel* result, const QString& msg, bool error)
{
    // The page-wide status line is at the very bottom, past every card: on a
    // scrolled page an error shown only there is invisible, and the button
    // looks broken. Put it in the card too, and colour a refusal red.
    if (result) {
        result->setStyleSheet(error ? "color:#ff6b6b;" : QString());
        result->setText(msg);
    }
    setStatus(msg, error);
}

void StakingPage::refresh()
{
    refreshRewards();

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
    if (m_registry_section) m_registry_section->setSummary(tr("%n staker(s)", "", int(keys.size())));

    refreshOwnStake(reg);
    refreshProducedBlocks();
    refreshWatchOnlyKey();
    refreshUnstakeInfo();
    refreshDelegation();
    refreshPoolOperator();

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


//! What staking has paid, and what the standing instruction would do with it.
//! Everything comes from the wallet RPCs, so the GUI and the command line can
//! never disagree about a staker's own money.
void StakingPage::refreshRewards()
{
    if (!m_rewards_table) return;
    if (!m_wallet_model) { m_rewards_table->setRowCount(0); return; }

    bool ok = false;
    QString err;
    const UniValue rewards = callRpc("liststakingrewards", UniValue(UniValue::VARR), ok, err, /*wallet=*/true);
    m_rewards_table->setRowCount(0);
    if (ok && rewards.isObject() && rewards["totals"].isArray()) {
        const UniValue& totals = rewards["totals"];
        for (size_t i = 0; i < totals.size(); ++i) {
            const UniValue& t = totals[i];
            const QString name = t["assetlabel"].isStr()
                ? QString::fromStdString(t["assetlabel"].get_str())
                : QString::fromStdString(t["asset"].get_str()).left(12) + QStringLiteral("…");
            const int row = m_rewards_table->rowCount();
            m_rewards_table->insertRow(row);
            m_rewards_table->setItem(row, 0, new QTableWidgetItem(name));
            m_rewards_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(t["mature"].getValStr())));
            m_rewards_table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(t["immature"].getValStr())));
        }
    }
    if (m_rewards_table->rowCount() == 0) {
        const int row = m_rewards_table->rowCount();
        m_rewards_table->insertRow(row);
        m_rewards_table->setItem(row, 0, new QTableWidgetItem(
            tr("No staking rewards yet. A block pays its own fees, so rewards appear once a block you "
               "(or your pool) produced carried some.")));
        m_rewards_table->setSpan(row, 0, 1, 3);
    }

    // The standing instruction, and a dry run of what it would do now.
    const UniValue ac = callRpc("getrewardautoconvert", UniValue(UniValue::VARR), ok, err, /*wallet=*/true);
    if (!ok || !ac.isObject()) {
        m_ac_status->setText(err.isEmpty() ? QString() : err);
        return;
    }

    const bool enabled = ac["enabled"].isBool() && ac["enabled"].get_bool();
    {
        const QSignalBlocker block(m_ac_enabled);
        m_ac_enabled->setChecked(enabled);
    }
    m_ac_form->setVisible(enabled);

    // The target picker: Bitcoin first -- the top seat is the whole of its
    // privilege here -- then whatever else this wallet holds.
    const QString target = ac["target"].isStr() ? QString::fromStdString(ac["target"].get_str()) : QStringLiteral("bitcoin");
    {
        const QSignalBlocker block(m_ac_target);
        m_ac_target->clear();
        m_ac_target->addItem(tr("%1 (Bitcoin)").arg(GUIUtil::parentBtcTicker()), QStringLiteral("bitcoin"));
        bool bok = false;
        QString berr;
        const UniValue bal = callRpc("getbalances", UniValue(UniValue::VARR), bok, berr, /*wallet=*/true);
        if (bok && bal.isObject() && bal["mine"].isObject() && bal["mine"]["trusted"].isObject()) {
            const UniValue& trusted = bal["mine"]["trusted"];
            for (const std::string& key : trusted.getKeys()) {
                m_ac_target->addItem(QString::fromStdString(key), QString::fromStdString(key));
            }
        }
        const int at = m_ac_target->findData(target);
        m_ac_target->setCurrentIndex(at >= 0 ? at : 0);
    }
    if (ac["min_receive"].isNum() || ac["min_receive"].isStr()) {
        m_ac_min->setText(QString::fromStdString(ac["min_receive"].getValStr()));
    }
    if (ac["max_slippage_bp"].isNum()) {
        const int at = m_ac_slippage->findData((int)ac["max_slippage_bp"].get_int64());
        if (at >= 0) m_ac_slippage->setCurrentIndex(at);
    }

    // What it decided, in the staker's own terms. "Nothing happened" and
    // "nothing should have happened" look identical otherwise, and the second
    // is by far the more common.
    QStringList lines;
    if (!enabled) {
        lines << tr("Rewards are being kept as they are.");
    } else if (ac["considered"].isArray() && ac["considered"].size() > 0) {
        const UniValue& rows = ac["considered"];
        for (size_t i = 0; i < rows.size(); ++i) {
            const UniValue& r = rows[i];
            const QString name = r["assetlabel"].isStr()
                ? QString::fromStdString(r["assetlabel"].get_str())
                : QString::fromStdString(r["asset"].get_str()).left(12) + QStringLiteral("…");
            lines << QStringLiteral("%1 %2 — %3")
                         .arg(QString::fromStdString(r["amount"].getValStr()), name,
                              QString::fromStdString(r["reason"].getValStr()));
        }
    } else {
        lines << tr("Nothing to convert right now. Rewards are gathered per asset until a batch is worth converting.");
    }
    if (ac["relay"].isStr() && ac["relay"].get_str().empty()) {
        lines << tr("No SeqDEX relay is configured, so nothing can be converted: set -seqoburl.");
    }
    m_ac_status->setText(lines.join(QStringLiteral("\n")));

    refreshSwaps();
}

//! Cross-chain conversions that have started and not finished.
//!
//! The staker is not being asked to do anything here. The wallet claims the
//! Bitcoin the moment the maker reveals the secret, and takes the asset back
//! when the timelock passes, whether or not anyone is watching. What this
//! answers is the question that arises anyway once an asset has left the
//! balance: where is it, and what happens if nothing else does.
void StakingPage::refreshSwaps()
{
    if (!m_swaps_box || !m_swaps_table) return;
    if (!m_wallet_model) { m_swaps_box->setVisible(false); return; }

    bool ok = false;
    QString err;
    const UniValue swaps = callRpc("listrewardswaps", UniValue(UniValue::VARR), ok, err, /*wallet=*/true);
    m_swaps_table->setRowCount(0);
    if (!ok || !swaps.isArray() || swaps.empty()) {
        m_swaps_box->setVisible(false);
        return;
    }

    for (size_t i = 0; i < swaps.size(); ++i) {
        const UniValue& sw = swaps[i];
        const QString asset = sw["assetlabel"].isStr()
            ? QString::fromStdString(sw["assetlabel"].get_str())
            : QString::fromStdString(sw["asset"].get_str()).left(12) + QStringLiteral("…");

        // The stage, said in terms of what has happened rather than in the
        // state machine's own words.
        const std::string state = sw["state"].isStr() ? sw["state"].get_str() : "";
        QString stage;
        if (state == "negotiating")      stage = tr("agreeing terms");
        else if (state == "btc_locked")  stage = tr("the maker has locked its Bitcoin");
        else if (state == "seq_funded")  stage = tr("your asset is locked, waiting on the maker");
        else                             stage = QString::fromStdString(state);

        // And what happens if nobody does anything at all. For a funded swap
        // that is the refund, which is the answer that actually reassures.
        QString outcome;
        if (sw["our_refund"].isObject()) {
            const int64_t to_go = sw["our_refund"]["blocks_to_go"].isNum()
                                      ? sw["our_refund"]["blocks_to_go"].get_int64() : 0;
            outcome = to_go > 0
                ? tr("your asset returns to you in %n block(s)", "", (int)to_go)
                : tr("your asset is due back now; the wallet is taking it");
        } else if (sw["maker_refund"].isObject()) {
            outcome = tr("nothing of yours is locked in this one yet");
        }
        if (sw["error"].isStr() && !sw["error"].get_str().empty()) {
            outcome += QStringLiteral(" — ") + QString::fromStdString(sw["error"].get_str());
        }

        const int row = m_swaps_table->rowCount();
        m_swaps_table->insertRow(row);
        m_swaps_table->setItem(row, 0, new QTableWidgetItem(
            QString::fromStdString(sw["amount"].getValStr()) + QStringLiteral(" ") + asset));
        m_swaps_table->setItem(row, 1, new QTableWidgetItem(
            QString::fromStdString(sw["btc_expected"].getValStr()) + QStringLiteral(" BTC")));
        m_swaps_table->setItem(row, 2, new QTableWidgetItem(stage));
        m_swaps_table->setItem(row, 3, new QTableWidgetItem(outcome));
    }

    m_swaps_intro->setText(tr("Converting into Bitcoin happens across two chains, so there is a stretch where "
                              "your asset is locked in a contract and the Bitcoin has not arrived. You do not "
                              "have to do anything: the wallet takes the Bitcoin as soon as the maker reveals "
                              "the secret, and takes your asset back if the maker never does."));
    m_swaps_box->setVisible(true);
}

void StakingPage::onResumeSwaps()
{
    if (!m_wallet_model) return;
    bool ok = false;
    QString err;
    m_swaps_resume->setEnabled(false);
    const UniValue left = callRpc("resumerewardswaps", UniValue(UniValue::VARR), ok, err, /*wallet=*/true);
    m_swaps_resume->setEnabled(true);
    if (!ok) {
        m_ac_status->setText(tr("Could not carry the swaps on: %1").arg(err));
        return;
    }
    if (left.isArray() && left.empty()) {
        m_ac_status->setText(tr("Every swap is finished."));
    }
    refreshSwaps();
}

void StakingPage::onRewardConvertToggled(bool on)
{
    bool ok = false;
    QString err;
    UniValue params(UniValue::VARR);
    params.push_back(on);
    callRpc("setrewardautoconvert", params, ok, err, /*wallet=*/true);
    if (!ok) {
        m_ac_status->setText(err);
        return;
    }
    refreshRewards();
}

void StakingPage::onRewardConvertSave()
{
    bool ok = false;
    QString err;
    UniValue params(UniValue::VARR);
    params.push_back(m_ac_enabled->isChecked());
    params.push_back(m_ac_target->currentData().toString().toStdString());
    const QString min = m_ac_min->text().trimmed();
    if (min.isEmpty()) {
        params.push_back(UniValue());
    } else {
        bool numeric = false;
        const double v = min.toDouble(&numeric);
        if (!numeric || v < 0) {
            m_ac_status->setText(tr("That is not an amount."));
            return;
        }
        params.push_back(UniValue(UniValue::VNUM, min.toStdString()));
    }
    params.push_back((int64_t)m_ac_slippage->currentData().toInt());
    callRpc("setrewardautoconvert", params, ok, err, /*wallet=*/true);
    m_ac_status->setText(ok ? tr("Saved.") : err);
    refreshRewards();
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
    m_registry_stake = mine;
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
    // Both cards are folded by default, so their headline number belongs in the
    // title -- otherwise "did I produce anything?" needs a click to answer.
    const QString produced_summary = produced > 0
        ? tr("%1 of the last %2 blocks").arg(produced).arg(scanned)
        : tr("none of the last %1 blocks").arg(scanned);
    if (m_production_section) m_production_section->setSummary(produced_summary);
    if (m_blocks_section) m_blocks_section->setSummary(produced_summary);
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
    if (amount.isEmpty()) { setCardResult(m_result, tr("Enter an amount of %1 to stake.").arg(BitcoinUnits::policyAssetTicker()), true); return; }
    bool amtok = false; const double amtval = amount.toDouble(&amtok);
    if (!amtok || amtval <= 0) { setCardResult(m_result, tr("Enter a positive %1 amount.").arg(BitcoinUnits::policyAssetTicker()), true); return; }
    // Reject sub-floor stakes up front: the consensus rule (and registerstake) drop
    // anything below the chain minimum, so it would never count toward production.
    if (g_pos_min_stake > 0) {
        const int64_t amt_sats = (int64_t)std::llround(amtval * 100000000.0);
        if (amt_sats < (int64_t)g_pos_min_stake) {
            setCardResult(m_result, tr("Minimum stake on this network is %1 %2 - a smaller stake would never count.")
                          .arg(QString::number((double)g_pos_min_stake / 100000000.0, 'f', 0), BitcoinUnits::policyAssetTicker()), true);
            return;
        }
    }
    bool ok; QString err;

    // 1) a fresh address whose key we'll stake with
    UniValue addrv = callRpc("getnewaddress", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setCardResult(m_result, tr("Could not create a staking address: %1").arg(err), true); return; }
    const QString addr = QString::fromStdString(addrv.getValStr());

    // 2) its public key
    UniValue aiparams(UniValue::VARR); aiparams.push_back(addr.toStdString());
    UniValue info = callRpc("getaddressinfo", aiparams, ok, err);
    if (!ok || !info.exists("pubkey")) { setCardResult(m_result, tr("Could not read the staking key: %1").arg(err), true); return; }
    const QString pubkey = QString::fromStdString(info["pubkey"].get_str());

    // 3) register the stake (funds the staking output)
    UniValue rsparams(UniValue::VARR);
    rsparams.push_back(pubkey.toStdString());
    rsparams.push_back(UniValue(UniValue::VNUM, amount.toStdString()));
    UniValue res = callRpc("registerstake", rsparams, ok, err);
    if (!ok) { setCardResult(m_result, tr("Staking failed: %1").arg(err), true); return; }
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
    // Clear any red left over from an earlier refusal: this is a success.
    m_result->setStyleSheet(QString());
    m_result->setText(msg);
    setStatus(enabled ? tr("Staked. Block production is on. The stake counts once the transaction confirms.")
                      : tr("Stake registered. It will count once the transaction confirms."), false);
    refresh();
}

void StakingPage::refreshUnstakeInfo(const UniValue* prefetched)
{
    if (!m_unstake_info || !m_wallet_model) return;
    bool ok = true; QString err;
    UniValue list = prefetched ? *prefetched : callRpc("liststakeutxos", UniValue(UniValue::VARR), ok, err);
    if (!ok || !list.isArray()) {
        m_unstake_info->setText(tr("Staked coins unavailable: %1").arg(err));
        if (m_unstake_button) m_unstake_button->setEnabled(false);
        if (m_unstake_button_holder) {
            m_unstake_button_holder->setToolTip(
                tr("Withdrawing is unavailable because the staked coins could not be read: %1").arg(err));
        }
        return;
    }
    CAmount mature = 0, immature = 0, withdrawing = 0;
    QString next_unlock;
    int64_t best_height = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const UniValue& o = list[i];
        CAmount amt = 0;
        try { amt = AmountFromValue(o["amount"]); } catch (...) { continue; }
        if (o["withdrawing"].isBool() && o["withdrawing"].get_bool()) {
            withdrawing += amt;
            continue;
        }
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
    if (withdrawing > 0 && mature == 0 && immature == 0) {
        // The commonest reason the numbers look wrong right after a withdrawal:
        // the coins are already spent, but the stake registry keeps crediting
        // their weight until the withdrawal confirms.
        text = tr("A withdrawal of %1 %2 has been sent and is waiting to confirm. Until it does, that amount "
                  "still counts as your stake.")
                   .arg(FormatWeight((uint64_t)withdrawing), ticker);
    } else if (mature == 0 && immature == 0 && m_registry_stake > 0) {
        // The registry credits this wallet's keys with stake, yet the wallet has
        // no staking output to spend. Saying "nothing is staked" here would
        // contradict the card above, which just showed that weight.
        text = tr("Your %1 %2 of registered stake cannot be withdrawn from this wallet, because it does not "
                  "have the transaction that created it — that stake was set up elsewhere.")
                   .arg(FormatWeight(m_registry_stake), ticker);
    } else if (mature == 0 && immature == 0) {
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
    // Offer the fee bump only while there is something to bump.
    if (m_unstake_bump) m_unstake_bump->setVisible(withdrawing > 0);
    // Say why the button is greyed out, right where the pointer already is: a
    // disabled control with no explanation reads as a broken one. The tooltip
    // goes on the enabled wrapper, since Qt sends no tooltip event to a
    // disabled widget.
    if (m_unstake_button_holder) {
        QString tip;
        if (mature > 0) {
            tip = tr("Withdraw %1 %2 back to this wallet.").arg(FormatWeight((uint64_t)mature), ticker);
        } else if (immature > 0) {
            tip = tr("Nothing can be withdrawn yet: your %1 %2 is still serving its unbonding wait (%3). "
                     "The stake keeps counting — and earning — the whole time.")
                      .arg(FormatWeight((uint64_t)immature), ticker, next_unlock);
        } else if (withdrawing > 0) {
            tip = tr("A withdrawal of %1 %2 is already on its way and waiting to confirm.")
                      .arg(FormatWeight((uint64_t)withdrawing), ticker);
        } else if (m_registry_stake > 0) {
            tip = tr("This wallet does not have the transaction that created your %1 %2 of registered "
                     "stake, so it cannot withdraw it.")
                      .arg(FormatWeight(m_registry_stake), ticker);
        } else {
            tip = tr("Nothing is staked from this wallet, so there is nothing to withdraw.");
        }
        m_unstake_button_holder->setToolTip(tip);
        if (m_unstake_button) m_unstake_button->setToolTip(tip);
    }
}

void StakingPage::onStakeMax()
{
    if (!m_wallet_model || !m_stake_amount) return;
    bool ok; QString err;
    UniValue bal = callRpc("getbalance", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setCardResult(m_result, tr("Could not read your balance: %1").arg(err), true); return; }
    // getbalance returns a map keyed by asset; the stake is always the policy asset.
    CAmount available = 0;
    if (bal.isObject()) {
        for (const std::string& k : bal.getKeys()) {
            if (k == "bitcoin" || k == ::policyAsset.GetHex()) {
                try { available = AmountFromValue(bal[k]); } catch (...) {}
                break;
            }
        }
    } else if (bal.isNum() || bal.isStr()) {
        try { available = AmountFromValue(bal); } catch (...) {}
    }
    // Leave headroom for the funding transaction's own fee. The exact fee is not
    // known until the wallet has picked the inputs, so keep a small, generous
    // margin rather than offer an amount that then fails to fund.
    const CAmount headroom = 10000; // 0.0001 SEQ
    if (available <= headroom) {
        setCardResult(m_result, tr("Your balance (%1 %2) is not enough to stake and still pay the transaction fee.")
                                     .arg(FormatWeight((uint64_t)std::max<CAmount>(available, 0)),
                                          BitcoinUnits::policyAssetTicker()), true);
        return;
    }
    const CAmount most = available - headroom;
    m_stake_amount->setText(FormatWeight((uint64_t)most));
    // Always say what Max decided. Silently filling a field leaves the user
    // wondering whether the click registered, and the reserved fee headroom is
    // exactly the thing that is not obvious.
    setCardResult(m_result, tr("Ready to stake %1 %2 — your whole balance except %3 %2, kept aside to pay the "
                              "transaction's network fee.")
                                .arg(FormatWeight((uint64_t)most), BitcoinUnits::policyAssetTicker(),
                                     FormatWeight((uint64_t)headroom)), false);
}

void StakingPage::onUnstakeMax()
{
    if (!m_wallet_model || !m_unstake_amount) return;
    // Show the number. Leaving the field empty already means "everything", but a
    // Max button that clears the box looks like it did nothing at all — the user
    // pressed it to SEE how much there is.
    bool ok; QString err;
    UniValue list = callRpc("liststakeutxos", UniValue(UniValue::VARR), ok, err);
    if (!ok || !list.isArray()) {
        setCardResult(m_unstake_result, tr("Could not read the staked coins: %1").arg(err), true);
        return;
    }
    CAmount mature = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        const UniValue& o = list[i];
        if (o["withdrawing"].isBool() && o["withdrawing"].get_bool()) continue;
        if (!(o["withdrawable"].isBool() && o["withdrawable"].get_bool())) continue;
        try { mature += AmountFromValue(o["amount"]); } catch (...) {}
    }
    // Keep the summary line consistent with the figure just filled in.
    refreshUnstakeInfo(&list);
    if (mature <= 0) {
        setCardResult(m_unstake_result, tr("There is nothing to withdraw right now."), true);
        return;
    }
    const QString ticker = BitcoinUnits::policyAssetTicker();
    m_unstake_amount->setText(FormatWeight((uint64_t)mature));
    setCardResult(m_unstake_result, tr("Ready to withdraw %1 %2 — everything currently available. The network "
                                       "fee is taken out of this amount.")
                                        .arg(FormatWeight((uint64_t)mature), ticker), false);
}

void StakingPage::onUnstakeBump()
{
    if (!m_wallet_model) return;
    const QString ticker = BitcoinUnits::policyAssetTicker();
    if (AskCentred(this, tr("Speed up the withdrawal?"),
                   tr("The withdrawal waiting to confirm will be re-sent with a higher network fee, so it is "
                      "picked up sooner.\n\nIt goes to the same address for the same amount, less the extra "
                      "fee. The original is replaced, not repeated — only one of the two can ever confirm.")) != QMessageBox::Yes) {
        return;
    }
    if (m_unstake_bump) m_unstake_bump->setEnabled(false);
    bool ok; QString err;
    UniValue res = callRpc("bumpwithdrawstakefee", UniValue(UniValue::VARR), ok, err);
    if (m_unstake_bump) m_unstake_bump->setEnabled(true);
    if (!ok) {
        setCardResult(m_unstake_result, tr("Could not speed up the withdrawal: %1").arg(err), true);
        return;
    }
    const QString txid = res.exists("txid") ? QString::fromStdString(res["txid"].getValStr()) : QString();
    const QString oldf = res.exists("old_fee") ? QString::fromStdString(res["old_fee"].getValStr()) : QString();
    const QString newf = res.exists("fee") ? QString::fromStdString(res["fee"].getValStr()) : QString();
    const QString amt = res.exists("amount") ? QString::fromStdString(res["amount"].getValStr()) : QString();
    if (m_unstake_result) {
        m_unstake_result->setStyleSheet(QString());
        m_unstake_result->setText(tr("Withdrawal re-sent with a higher fee: %1 %2 instead of %3 %2.\n"
                                     "You now receive %4 %2.\nTransaction: %5")
                                      .arg(newf, ticker, oldf, amt, txid));
    }
    setStatus(tr("Withdrawal re-sent with a higher fee."), false);
    refresh();
}

void StakingPage::onUnstake()
{
    if (!m_wallet_model) return;
    bool ok; QString err;

    // What the wallet has staked, and how much of it is withdrawable right now.
    UniValue list = callRpc("liststakeutxos", UniValue(UniValue::VARR), ok, err);
    if (!ok || !list.isArray()) { setCardResult(m_unstake_result, tr("Could not read the staked coins: %1").arg(err), true); return; }
    CAmount mature_total = 0, immature_total = 0, withdrawing_total = 0;
    QString next_unlock;
    int64_t best_height = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const UniValue& o = list[i];
        CAmount amt = 0;
        try { amt = AmountFromValue(o["amount"]); } catch (...) { continue; }
        // Already on its way out: neither withdrawable again nor something the
        // user is waiting to unlock.
        if (o["withdrawing"].isBool() && o["withdrawing"].get_bool()) {
            withdrawing_total += amt;
            continue;
        }
        if (o["withdrawable"].isBool() && o["withdrawable"].get_bool()) {
            mature_total += amt;
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
    // The card's summary line was rendered at the last refresh and the numbers
    // move with every block; re-render it from the very list just fetched, so
    // the card and any refusal below can never quote two different totals.
    refreshUnstakeInfo(&list);
    if (mature_total == 0 && immature_total == 0) {
        setCardResult(m_unstake_result, withdrawing_total > 0
            ? tr("A withdrawal of %1 %2 is already on its way; wait for it to confirm.")
                  .arg(FormatWeight((uint64_t)withdrawing_total), ticker)
            : tr("Nothing is staked from this wallet."), true);
        return;
    }
    if (mature_total == 0) {
        setCardResult(m_unstake_result, tr("Nothing can be withdrawn yet: %1.").arg(next_unlock), true);
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
            setCardResult(m_unstake_result, tr("Enter a positive %1 amount, or leave the field empty to withdraw everything available.").arg(ticker), true);
            return;
        }
        want = (CAmount)std::llround(amtval * 100000000.0);
        if (want > mature_total) {
            setCardResult(m_unstake_result, tr("Only %1 %2 can be withdrawn right now%3").arg(
                          FormatWeight((uint64_t)mature_total), ticker,
                          immature_total > 0 ? tr(" — the rest is still unbonding.") : QString(".")), true);
            return;
        }
        partial = want < mature_total;
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
    if (AskCentred(this, tr("Withdraw stake?"), msg) != QMessageBox::Yes) {
        return;
    }

    if (m_unstake_button) m_unstake_button->setEnabled(false);
    UniValue params(UniValue::VARR);
    if (partial) {
        // Any staker key may contribute (null pubkey): the RPC takes whole
        // outputs largest-first, so at most one is split and its remainder is
        // re-staked to that same key. The amount goes as typed, so the RPC
        // parses it once and no double rounding creeps in.
        params.push_back(UniValue(UniValue::VNULL));
        params.push_back(UniValue(UniValue::VNUM, amount_text.toStdString()));
    }
    UniValue res = callRpc("withdrawstake", params, ok, err);
    if (m_unstake_button) m_unstake_button->setEnabled(true);
    if (!ok) {
        setCardResult(m_unstake_result, tr("The withdrawal failed: %1").arg(err), true);
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
    // The card already carries the detailed outcome above; keep the page-wide
    // line for the one-sentence confirmation so it does not overwrite it.
    if (m_unstake_result) {
        m_unstake_result->setStyleSheet(QString());
        m_unstake_result->setText(out);
    }
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

void StakingPage::refreshDelegation()
{
    if (!m_deleg_status || !m_wallet_model) return;
    bool ok = false; QString err;

    // Where this wallet's weight is signing, and what the pool holding it has
    // committed to. listdelegations carries the alerts: it is the RPC that
    // knows both sides (our keys, and the pool's on-chain commitments).
    UniValue mine = callRpc("listdelegations", UniValue(UniValue::VARR), ok, err);
    if (!ok || !mine.isArray()) {
        m_deleg_status->setText(tr("Delegation status unavailable: %1").arg(err));
        return;
    }

    QStringList alerts;
    QString status;
    int delegated_rows = 0;
    for (size_t i = 0; i < mine.size(); ++i) {
        const UniValue& row = mine[i];
        if (row["alerts"].isArray()) {
            for (size_t j = 0; j < row["alerts"].size(); ++j) {
                alerts << QString::fromStdString(row["alerts"][j].get_str());
            }
        }
        if (!row["delegated"].isBool() || !row["delegated"].get_bool()) continue;
        ++delegated_rows;
        const QString signer = QString::fromStdString(row["signer"].getValStr());
        const double weight = row["weight"].isNum() ? (double)row["weight"].get_int64() / 100000000.0 : 0.0;
        const double share = row["pool_share"].isNum() ? row["pool_share"].get_real() * 100.0 : 0.0;
        status += tr("Delegated: %1 %2 of your stake signs for pool %3, which is %4% of that pool's weight.\n")
                      .arg(QString::number(weight, 'f', 2), BitcoinUnits::policyAssetTicker(),
                           signer, QString::number(share, 'f', 2));
        if (row["policy_in_force"].isObject()) {
            const UniValue& p = row["policy_in_force"];
            if (p["mode"].getValStr() == "lottery") {
                status += tr("It pays one delegator per block, drawn by stake weight, keeping %1% commission. "
                             "You earn your exact share over time, in occasional lumps rather than steadily.\n")
                              .arg(QString::number(p["commission_bp"].get_int64() / 100.0, 'f', 2));
            } else {
                status += tr("It pays a committed address on every block. The chain stops it redirecting the "
                             "reward silently, but does not check that address shares anything with you.\n");
            }
        }
    }
    if (mine.empty()) {
        status = tr("No stake registered yet, so there is nothing to delegate. Stake above first.");
    } else if (delegated_rows == 0) {
        status = tr("Not delegating: your stake signs for itself, and this wallet must be online and producing "
                    "to earn from it.");
    }
    m_deleg_status->setText(status.trimmed());

    // The watch, shouted rather than filed: a pool's announced change is only a
    // protection if the delegator sees it while there is still time to leave.
    if (alerts.isEmpty()) {
        m_deleg_alerts->setVisible(false);
    } else {
        m_deleg_alerts->setText(QStringLiteral("⚠ ") + alerts.join(QStringLiteral("\n\n⚠ ")));
        m_deleg_alerts->setStyleSheet("QLabel{padding:8px;border-radius:4px;background:#fff3cd;color:#856404;}");
        m_deleg_alerts->setVisible(true);
    }
    m_undeleg_button->setEnabled(delegated_rows > 0);

    if (m_pool_section) {
        // An alert is the one thing a folded card must not hide: a pool's
        // announced change only protects the delegator who sees it while there
        // is still time to leave. So it opens the card rather than waiting to
        // be clicked, and says so in the title either way.
        m_pool_section->setSummary(!alerts.isEmpty()
            ? tr("⚠ %n alert(s)", "", alerts.size())
            : (delegated_rows > 0 ? tr("delegated") : tr("not delegating")));
        if (!alerts.isEmpty()) m_pool_section->setOpen(true);
    }
}

void StakingPage::onDelegate()
{
    if (!m_wallet_model) return;
    const QString signer = m_deleg_signer->text().trimmed();
    if (signer.isEmpty()) {
        setCardResult(m_deleg_result, tr("Pick a pool from the table, or paste its signer public key."), true);
        return;
    }

    // Say what is actually at stake before doing it. The one thing a delegator
    // must understand is that the coins do not move and cannot be taken -- and
    // the one thing they must check is what the pool has committed to.
    const QMessageBox::StandardButton confirmed = QMessageBox::question(
        this, tr("Delegate to this pool?"),
        tr("Your stake's block-signing rights go to:\n\n%1\n\n"
           "Your coins do NOT move, and this pool can never spend them — only this wallet can. "
           "You can take the rights back at any time, immediately and without the pool's cooperation.\n\n"
           "What you are trusting it for is the reward: check the \"Pays out\" column first. A pool that has "
           "committed to nothing keeps everything the blocks it produces earn.\n\nDelegate now?").arg(signer),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirmed != QMessageBox::Yes) return;

    const QString amount = m_deleg_amount->text().trimmed();
    bool ok = false; QString err;
    UniValue params(UniValue::VARR);
    params.push_back(signer.toStdString());
    // Empty means "lend what I already stake"; a figure means "stake this much
    // and lend it", which is the path for anyone who does not hold enough to
    // stake alone.
    if (amount.isEmpty()) params.push_back(UniValue(UniValue::VNULL));
    else params.push_back(UniValue(UniValue::VNUM, amount.toStdString()));
    UniValue res = callRpc("delegatestake", params, ok, err);
    if (!ok) { setCardResult(m_deleg_result, tr("Delegating failed: %1").arg(err), true); return; }

    const QString txid = QString::fromStdString(res["txid"].getValStr());
    const double weight = res["delegated_weight"].isNum() ? (double)res["delegated_weight"].get_int64() / 100000000.0 : 0.0;
    QString msg = res.exists("previous_signer")
        ? tr("Re-pointed %1 %2 of stake weight to %3.\nTransaction: %4")
              .arg(QString::number(weight, 'f', 2), BitcoinUnits::policyAssetTicker(), signer, txid)
        : tr("Delegated %1 %2 of stake weight to %3.\nTransaction: %4")
              .arg(QString::number(weight, 'f', 2), BitcoinUnits::policyAssetTicker(), signer, txid);
    if (res.exists("staked")) {
        msg += tr("\nStaked %1 %2 and lent it in the same transaction.")
                   .arg(QString::fromStdString(res["staked"].getValStr()), BitcoinUnits::policyAssetTicker());
    }
    msg += tr("\nIt takes effect when this confirms. Your coins stay yours: leaving spends only the record.");
    if (res.exists("note")) msg += QStringLiteral("\n") + QString::fromStdString(res["note"].getValStr());
    m_deleg_result->setStyleSheet(QString());
    m_deleg_result->setText(msg);
    setStatus(tr("Delegation sent. It takes effect once the transaction confirms."), false);
    refresh();
}

void StakingPage::onUndelegate()
{
    if (!m_wallet_model) return;
    const QMessageBox::StandardButton confirmed = QMessageBox::question(
        this, tr("Take your stake back?"),
        tr("Your stake's weight will count for you again from the next confirmation, and the pool loses it.\n\n"
           "This does NOT unstake: your coins were never moved by delegating and are not moved now. To unstake, "
           "use \"Withdraw stake\" above.\n\nAfterwards this wallet must be online and producing to earn from "
           "the stake itself.\n\nTake it back?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirmed != QMessageBox::Yes) return;

    bool ok = false; QString err;
    UniValue res = callRpc("undelegatestake", UniValue(UniValue::VARR), ok, err);
    if (!ok) { setCardResult(m_deleg_result, tr("Could not take the stake back: %1").arg(err), true); return; }
    m_deleg_result->setStyleSheet(QString());
    m_deleg_result->setText(tr("Reclaimed. Transaction: %1\nYour weight counts for you again once it confirms.")
                                .arg(QString::fromStdString(res["txid"].getValStr())));
    setStatus(tr("Stake reclaimed. It counts for you again once the transaction confirms."), false);
    refresh();
}

void StakingPage::refreshPoolOperator()
{
    if (!m_pool_status || !m_wallet_model) return;

    // Ask about THIS node's own signer keys by name, one at a time. Two reasons
    // not to fetch the list and filter it: a signer that has not announced a
    // payout policy is not a pool and is absent from an unfiltered listing, so
    // an operator with stake but no commitment yet would be told it had none;
    // and this card is about your own numbers, not a directory of everyone
    // else's, which belongs on the public board.
    QString status, commitment;
    bool any_mine = false;
    QStringList mine_keys;
    bool ok = false; QString err;
    for (const std::string& signer : m_my_pubkeys) {
        UniValue pparams(UniValue::VARR);
        pparams.push_back(signer);
        pparams.push_back(500);
        UniValue pools = callRpc("listpools", pparams, ok, err, /*wallet=*/false);
        if (!ok || !pools["pools"].isArray() || pools["pools"].empty()) continue;
        const UniValue& p = pools["pools"][0];
        {
            any_mine = true;
        mine_keys << QString::fromStdString(signer);
        const QString key = QString::fromStdString(signer);
        const double weight = (double)p["weight"].get_int64() / 100000000.0;
        const double lent = (double)p["delegated_weight"].get_int64() / 100000000.0;
        const int64_t delegators = p["delegators"].get_int64();
        status += tr("Your key %1 commands %2 %3 (%4% of the network), of which %5 was lent to you by %6 "
                     "delegator(s).\n")
                      .arg(key.left(10) + QStringLiteral("…"), QString::number(weight, 'f', 2),
                           BitcoinUnits::policyAssetTicker(),
                           QString::number(p["network_share"].get_real() * 100.0, 'f', 2),
                           QString::number(lent, 'f', 2), QString::number(delegators));
        if (p["reliability"].isNum()) {
            const double rel = p["reliability"].get_real();
            status += tr("Recent production: %1 blocks against the %2 your weight was owed (%3).\n")
                          .arg(QString::number(p["blocks_produced"].get_int64()),
                               QString::number(p["blocks_expected"].get_real(), 'f', 1),
                               rel >= 0.85 ? tr("on target")
                                           : tr("BELOW target: check this node is online and producing"));
        }
        if (!p["eligible"].isNull() && !p["eligible"].get_bool()) {
            status += tr("This key is below the chain's minimum stake, so it cannot produce at all yet.\n");
        }
        if (p["policy_in_force"].isObject()) {
            const UniValue& q = p["policy_in_force"];
            commitment += q["mode"].getValStr() == "lottery"
                ? tr("Committed: lottery, you keep %1%. Binding since height %2.\n")
                      .arg(QString::number(q["commission_bp"].get_int64() / 100.0, 'f', 2),
                           QString::number(q["activation"].get_int64()))
                : tr("Committed: direct, every block pays a fixed script. Binding since height %1.\n")
                      .arg(QString::number(q["activation"].get_int64()));
        } else {
            commitment += tr("Committed: nothing, so you keep every fee your blocks earn. You are a staker, not "
                             "a pool: until you announce a policy below you are not listed on the pool board and "
                             "nobody is being asked to delegate to you.\n");
        }
        if (p["policy_pending"].isArray()) {
            for (size_t j = 0; j < p["policy_pending"].size(); ++j) {
                const UniValue& q = p["policy_pending"][j];
                commitment += tr("Announced, binds at height %1 (%2 blocks away). Your delegators can see it and "
                                 "leave until then.\n")
                                  .arg(QString::number(q["activation"].get_int64()),
                                       QString::number(q["blocks_away"].get_int64()));
            }
        }
        }
    }
    if (!any_mine) {
        status = tr("This wallet has no staking key registered, so it is not a pool yet. Stake above first: "
                    "a pool is simply a staker that others have lent their weight to.");
    }
    m_pool_status->setText(status.trimmed());
    m_pool_commitment->setText(commitment.trimmed());

    // With one key the RPC picks it and the row is noise; with several it cannot
    // guess, and without the row the card would refuse every announcement.
    if (m_payout_signer) {
        const QString kept = m_payout_signer->currentText();
        m_payout_signer->clear();
        m_payout_signer->addItems(mine_keys);
        const int at = m_payout_signer->findText(kept);
        if (at >= 0) m_payout_signer->setCurrentIndex(at);
        const bool many = mine_keys.size() > 1;
        m_payout_signer->setVisible(many);
        m_payout_signer_label->setVisible(many);
    }
    if (m_payout_button) m_payout_button->setEnabled(any_mine);
    onPayoutPresetChanged();
}

//! "split", "lottery" or "direct" -- what the chosen arrangement announces.
QString StakingPage::payoutModeName() const
{
    if (!m_payout_preset) return QStringLiteral("split");
    const QString data = m_payout_preset->currentData().toString();
    if (data == QLatin1String("custom")) return QStringLiteral("direct"); // a raw script IS a direct commitment
    return data.section(QLatin1Char(':'), 0, 0);
}

//! The commission the chosen arrangement carries, in basis points, or -1 when
//! the operator's own figure is not a percentage.
int64_t StakingPage::payoutCommissionBp() const
{
    if (!m_payout_preset) return 0;
    const QString data = m_payout_preset->currentData().toString();
    if (data.endsWith(QLatin1String(":ask"))) {
        if (!m_payout_commission) return 0;
        const QString txt = m_payout_commission->text().trimmed();
        if (txt.isEmpty()) return -1;
        bool ok = false;
        const double pct = txt.toDouble(&ok);
        if (!ok || pct < 0 || pct > 100) return -1;
        return (int64_t)std::llround(pct * 100.0);
    }
    const QStringList parts = data.split(QLatin1Char(':'));
    return parts.size() == 2 && parts.at(1) != QLatin1String("ask") ? parts.at(1).toLongLong() : 0;
}

void StakingPage::onPayoutPresetChanged()
{
    if (!m_payout_preset) return;
    const QString data = m_payout_preset->currentData().toString();
    const bool custom = data == QLatin1String("custom");
    const bool ask = data.endsWith(QLatin1String(":ask"));
    const bool direct_addr = data == QLatin1String("direct:0");

    // Each arrangement asks for exactly what it needs and nothing else.
    m_payout_commission->setVisible(ask);
    m_payout_commission_label->setVisible(ask);
    m_payout_address->setVisible(direct_addr);
    m_payout_address_label->setVisible(direct_addr);
    m_payout_script->setVisible(custom);
    m_payout_script_label->setVisible(custom);
    m_payout_activation->setVisible(custom);
    m_payout_activation_label->setVisible(custom);

    // Say what the DELEGATOR experiences, which is what the operator is choosing
    // between and what they will be judged on.
    QString note;
    if (custom) {
        note = tr("Commit an arbitrary scriptPubKey: every block you produce must pay it, byte for byte. For an "
                  "arrangement an address cannot express, such as a multisig, a covenant, or a contract that "
                  "splits the reward. The chain checks only that the coinbase pays these bytes, so whether they "
                  "are spendable, and by whom, is entirely yours to get right.");
    } else if (direct_addr) {
        note = tr("Every block you produce must pay the address you commit to. The chain stops you redirecting "
                  "that reward silently, but it does NOT check the address shares anything with your delegators, "
                  "and they can see that. Choose this when you are paying out by some arrangement of your own.");
    } else if (payoutModeName() == QLatin1String("lottery")) {
        note = tr("Every block you produce pays ONE of your delegators, drawn by stake weight from Bitcoin's "
                  "proof of work so the draw cannot be rigged, and you keep the commission you set below as a "
                  "share of blocks. Each delegator earns its exact share over time, arriving in occasional "
                  "lumps rather than steadily. Most pools want the proportional arrangements above instead.");
    } else {
        const int64_t bp = payoutCommissionBp();
        const QString share = ask
            ? tr("the commission you set below")
            : tr("%1%").arg(QString::number(bp / 100.0, 'f', bp % 100 ? 2 : 0));
        note = bp == 0 && !ask
            ? tr("Your rewards pool up on-chain, and anyone may trigger the payout, which sends every delegator "
                 "its exact proportional share -- the arrangement most delegators expect. You take nothing "
                 "beyond what your own stake earns as one participant among them. Shares too small to pay "
                 "simply wait for the next payout instead of being rounded away.")
            : tr("Your rewards pool up on-chain, and anyone may trigger the payout, which sends every delegator "
                 "its exact proportional share -- the arrangement most delegators expect. You keep %1 as "
                 "commission. Shares too small to pay simply wait for the next payout instead of being rounded "
                 "away.").arg(share);
    }
    m_payout_preset_note->setText(note);
}

void StakingPage::onAnnouncePayout()
{
    if (!m_wallet_model) return;
    const QString mode = payoutModeName();
    const bool shares_commission = mode != QLatin1String("direct");

    int64_t commission_bp = 0;
    if (shares_commission) {
        commission_bp = payoutCommissionBp();
        if (commission_bp < 0) {
            setCardResult(m_payout_result, tr("Commission must be between 0 and 100 percent."), true);
            return;
        }
    }

    // Say plainly what is being signed up to, because it binds every future
    // block and cannot be revoked in less than the notice period.
    const QMessageBox::StandardButton confirmed = QMessageBox::question(
        this, tr("Announce this payout policy?"),
        (mode == QLatin1String("split")
            ? tr("From the activation height on, every block you produce pays into your pool's pot, and anyone "
                 "may trigger the payout that sends every delegator its exact proportional share. You keep %1% "
                 "as commission. The chain enforces all of it: a block of yours that pays anywhere else is "
                 "invalid, and so is a payout that shortchanges anyone.\n\n").arg(QString::number(commission_bp / 100.0, 'f', 2))
        : mode == QLatin1String("lottery")
            ? tr("From the activation height on, every block you produce must pay one of your delegators, drawn "
                 "by stake weight, and you keep %1% of blocks as commission. The chain enforces this: a block of "
                 "yours that pays anyone else is invalid.\n\n").arg(QString::number(commission_bp / 100.0, 'f', 2))
            : (m_payout_preset->currentData().toString() == QLatin1String("custom")
                ? tr("From the activation height on, every block you produce must pay the script you entered, "
                     "byte for byte. The chain checks only that, so if those bytes cannot be spent by anyone, "
                     "every reward this key earns is burned and the chain will enforce that just as "
                     "faithfully.\n\n")
                : tr("From the activation height on, every block you produce must pay the committed address. The "
                     "chain enforces that, but it does not check the address shares anything with your delegators, "
                     "and they can see that.\n\n")))
        + tr("It cannot take effect immediately: the chain requires a notice period first, so your delegators "
             "can read it and leave before it binds. Until then your current arrangement stands.\n\nAnnounce it?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirmed != QMessageBox::Yes) return;

    UniValue params(UniValue::VARR);
    params.push_back(mode.toStdString());
    // Empty means "this wallet's staker key", which the RPC resolves when there
    // is exactly one; the selector is only populated when there is more.
    const QString chosen = (m_payout_signer && m_payout_signer->isVisible())
                               ? m_payout_signer->currentText().trimmed() : QString();
    params.push_back(chosen.isEmpty() ? UniValue(UniValue::VNULL) : UniValue(chosen.toStdString()));

    const bool custom = m_payout_preset->currentData().toString() == QLatin1String("custom");
    const QString activation = custom ? m_payout_activation->text().trimmed() : QString();
    params.push_back(activation.isEmpty() ? UniValue(UniValue::VNULL)
                                          : UniValue(UniValue::VNUM, activation.toStdString()));
    if (shares_commission) {
        params.push_back(UniValue(UniValue::VNULL));            // address: not used
        params.push_back(commission_bp);
    } else {
        const QString addr = custom ? QString() : m_payout_address->text().trimmed();
        params.push_back(addr.isEmpty() ? UniValue(UniValue::VNULL) : UniValue(addr.toStdString()));
        if (custom) {
            const QString script = m_payout_script->text().trimmed();
            if (script.isEmpty()) {
                setCardResult(m_payout_result, tr("Enter the payout script to commit to, in hex."), true);
                return;
            }
            params.push_back(UniValue(UniValue::VNULL));   // commission_bp: lottery only
            params.push_back(UniValue(UniValue::VNULL));   // amount: default
            params.push_back(script.toStdString());        // payout_script
        }
    }

    bool ok = false; QString err;
    UniValue res = callRpc("announcepayout", params, ok, err);
    if (!ok) { setCardResult(m_payout_result, tr("Could not announce: %1").arg(err), true); return; }

    QString msg = tr("Announced. Transaction: %1\nIt binds at height %2 (the notice period is %3 blocks).")
                      .arg(QString::fromStdString(res["txid"].getValStr()),
                           QString::number(res["activation"].get_int64()),
                           QString::number(res["notice_blocks"].get_int64()));
    if (res.exists("address")) {
        msg += tr("\nEvery block will pay: %1").arg(QString::fromStdString(res["address"].getValStr()));
    }
    msg += tr("\nUntil that height your delegators can read it and leave, which is what the wait is for.");
    m_payout_result->setStyleSheet(QString());
    m_payout_result->setText(msg);
    setStatus(tr("Payout policy announced. It binds after the notice period."), false);
    refresh();
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
