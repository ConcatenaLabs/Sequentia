// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <policy/policy.h>
#include <univalue.h>
#include <util/system.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStatusTipEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <thread>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle *_platformStyle, QObject *parent=nullptr):
        QAbstractItemDelegate(parent), unit(BitcoinUnits::BTC),
        platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        if (index.data(TransactionTableModel::WatchonlyRole).toBool()) {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(addressRect.left(), addressRect.top(), 16, addressRect.height());
            iconWatchonly = platformStyle->TextColorIcon(iconWatchonly);
            iconWatchonly.paint(painter, watchonlyRect);
            addressRect.setLeft(addressRect.left() + watchonlyRect.width() + 5);
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = index.sibling(index.row(), TransactionTableModel::Amount).data(Qt::DisplayRole).toString();

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    int unit;

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(nullptr),
    walletModel(nullptr),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    m_balances.balance[::policyAsset] = -1;

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);

    // --- Sequentia network status panel (Bitcoin anchor + staking / producer) ---
    {
        QFrame* seqFrame = new QFrame(this);
        seqFrame->setFrameShape(QFrame::StyledPanel);
        QVBoxLayout* seqLayout = new QVBoxLayout(seqFrame);
        seqLayout->setContentsMargins(10, 6, 10, 6);
        QLabel* seqTitle = new QLabel(tr("Sequentia network"), seqFrame);
        QFont stf = seqTitle->font(); stf.setBold(true); seqTitle->setFont(stf);
        seqLayout->addWidget(seqTitle);
        m_anchor_label = new QLabel(tr("Bitcoin anchor: ..."), seqFrame);
        m_anchor_label->setWordWrap(true);
        m_anchor_label->setToolTip(tr("Every Sequentia block points at a recent Bitcoin block — its \"anchor\". "
                                      "This borrows Bitcoin's ordering of history: to rewrite Sequentia you would "
                                      "have to rewrite Bitcoin. When Bitcoin is settling a change of its own, new "
                                      "Sequentia blocks pause briefly and resume on their own."));
        seqLayout->addWidget(m_anchor_label);
        m_staking_label = new QLabel(tr("Staking: ..."), seqFrame);
        m_staking_label->setWordWrap(true);
        m_staking_label->setToolTip(tr("Staking makes this node a block producer: the more %1 you stake, the more "
                                       "often you are chosen to produce a block and collect its fees. The stake "
                                       "stays yours the whole time. Set it up in the Staking tab.")
                                        .arg(BitcoinUnits::policyAssetTicker()));
        seqLayout->addWidget(m_staking_label);
        m_finality_label = new QLabel(tr("Finality: ..."), seqFrame);
        m_finality_label->setWordWrap(true);
        m_finality_label->setToolTip(tr("A checkpoint of Sequentia's history is written on Bitcoin. Once it is "
                                        "buried deep enough there, everything below it is final: it can no longer "
                                        "change, no matter what happens on Bitcoin above it."));
        seqLayout->addWidget(m_finality_label);
        if (QVBoxLayout* top = qobject_cast<QVBoxLayout*>(layout())) {
            top->insertWidget(1, seqFrame); // after labelAlerts (index 0), above the balances row
        }
    }

    // Sequentia: what the whole wallet is worth, above the per-asset rows. With
    // several assets in play a bare list of amounts answers "how much of each",
    // never "how much do I have" — that is this line's job.
    {
        m_total_value = new QLabel(ui->frame);
        QFont f = m_total_value->font();
        f.setPointSizeF(f.pointSizeF() * 1.9);
        f.setBold(true);
        m_total_value->setFont(f);
        m_total_value->setToolTip(tr("Everything in this wallet, valued at the latest prices the node has. "
                                     "Assets with no published price are not counted."));
        m_total_value->setVisible(false);
        // Row 1 of the panel: under the "Balances" title, above the amounts.
        ui->verticalLayout_4->insertWidget(1, m_total_value);
    }

    // Parent-chain (Bitcoin testnet4) balance, shown inside the Balances panel rather
    // than the network-status panel: a Sequentia receiving address is ALSO a Bitcoin
    // testnet4 address, so the same address can hold real tBTC. It is scanned from the
    // parent chain (not part of the Sequentia wallet balance), so it sits on its own
    // separated row below the asset balances, with the dual-address note as a tooltip.
    {
        QFrame* btcSep = new QFrame(ui->frame);
        btcSep->setFrameShape(QFrame::HLine);
        btcSep->setFrameShadow(QFrame::Sunken);
        ui->verticalLayout_4->addWidget(btcSep);
        m_btc_label = new QLabel(tr("Bitcoin (testnet4): loading..."), ui->frame);
        m_btc_label->setWordWrap(true);
        m_btc_label->setStyleSheet("color:#9b988e;");
        m_btc_label->setToolTip(tr("Your Sequentia receiving address is also a Bitcoin testnet4 address, so the same "
                                   "address can hold real testnet Bitcoin (tBTC). This amount is scanned from the "
                                   "Bitcoin testnet4 chain; it is not a Sequentia balance and spending it requires a "
                                   "Bitcoin node."));
        ui->verticalLayout_4->addWidget(m_btc_label);
    }
    m_seq_status_timer = new QTimer(this);
    m_seq_status_timer->setInterval(8000);
    connect(m_seq_status_timer, &QTimer::timeout, this, &OverviewPage::updateSeqStatus);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    if (m_balances.balance[::policyAsset] != -1) {
        setBalance(m_balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;

    // SEQUENTIA: each asset line carries its own muted "≈ <value> <REF>" in the chosen
    // reference currency. In privacy mode we fall back to the plain (value-less) format so
    // the masked balance isn't leaked via the ≈.
    const QString refCur = walletModel->getOptionsModel()->getReferenceCurrency();
    auto perAsset = [&](const CAmountMap& m) -> QString {
        if (m_privacy)
            return GUIUtil::formatMultiAssetAmount(m, unit, BitcoinUnits::SeparatorStyle::ALWAYS, "\n");
        return GUIUtil::formatMultiAssetAmountWithValue(m, unit, BitcoinUnits::SeparatorStyle::ALWAYS, refCur, "\n");
    };
    // Total rows: per-asset values, plus a summed grand total — but only when 2+ assets are
    // valued, otherwise the grand total just repeats the single asset's per-line value.
    auto withTotalValue = [&](const CAmountMap& m) -> QString {
        const QString s = perAsset(m);
        if (m_privacy) return s;
        int valued = 0;
        for (const auto& it : m) if (it.second > 0) ++valued;
        if (valued < 2) return s;
        const QString r = GUIUtil::formatMultiAssetReferenceApprox(m, refCur);
        return r.isEmpty() ? s : (s + "\n" + r);
    };

    // The headline: everything spendable plus everything still confirming, valued
    // in one number. Hidden in privacy mode (it would unmask the masked rows) and
    // whenever no asset in the wallet has a price to value it with.
    if (m_total_value) {
        const CAmountMap all = walletModel->wallet().privateKeysDisabled()
            ? balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance
            : balances.balance + balances.unconfirmed_balance + balances.immature_balance;
        const QString total = m_privacy ? QString() : GUIUtil::formatMultiAssetReferenceApprox(all, refCur);
        m_total_value->setText(total);
        m_total_value->setVisible(!total.isEmpty());
    }

    if (walletModel->wallet().isLegacy()) {
        if (walletModel->wallet().privateKeysDisabled()) {
            ui->labelBalance->setText(perAsset(balances.watch_only_balance));
            ui->labelUnconfirmed->setText(perAsset(balances.unconfirmed_watch_only_balance));
            ui->labelImmature->setText(perAsset(balances.immature_watch_only_balance));
            ui->labelTotal->setText(withTotalValue(balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance));
        } else {
            ui->labelBalance->setText(perAsset(balances.balance));
            ui->labelUnconfirmed->setText(perAsset(balances.unconfirmed_balance));
            ui->labelImmature->setText(perAsset(balances.immature_balance));
            ui->labelTotal->setText(withTotalValue(balances.balance + balances.unconfirmed_balance + balances.immature_balance));
            ui->labelWatchAvailable->setText(perAsset(balances.watch_only_balance));
            ui->labelWatchPending->setText(perAsset(balances.unconfirmed_watch_only_balance));
            ui->labelWatchImmature->setText(perAsset(balances.immature_watch_only_balance));
            ui->labelWatchTotal->setText(withTotalValue(balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance));
        }
    } else {
        ui->labelBalance->setText(perAsset(balances.balance));
        ui->labelUnconfirmed->setText(perAsset(balances.unconfirmed_balance));
        ui->labelImmature->setText(perAsset(balances.immature_balance));
        ui->labelTotal->setText(withTotalValue(balances.balance + balances.unconfirmed_balance + balances.immature_balance));
    }
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = !!balances.immature_balance;
    bool showWatchOnlyImmature = !!balances.immature_watch_only_balance;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(!walletModel->wallet().privateKeysDisabled() && showWatchOnlyImmature); // show watch-only immature balance

    // Resize the QScrollArea content widget
    QSize grid_size = ui->gridLayout->sizeHint();
    ui->scrollAreaWidgetContents->setMinimumSize(grid_size);
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly)
        ui->labelWatchImmature->hide();
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::useEmbeddedMonospacedFontChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(model->getOptionsModel()->getUseEmbeddedMonospacedFont());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);
        connect(model->getOptionsModel(), &OptionsModel::referenceCurrencyChanged, this, &OverviewPage::updateDisplayUnit);

        updateWatchOnlyLabels(wallet.haveWatchOnly() && !model->wallet().privateKeysDisabled());
        connect(model, &WalletModel::notifyWatchonlyChanged, [this](bool showWatchOnly) {
            updateWatchOnlyLabels(showWatchOnly && !walletModel->wallet().privateKeysDisabled());
        });
    }

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();

    // Kick off the Sequentia network-status panel (anchor + staking)
    if (m_seq_status_timer && walletModel) {
        updateSeqStatus();
        m_seq_status_timer->start();
        refreshBtcBalance(); // auto-load the dual (tBTC) balance; no manual button
    }
}

void OverviewPage::updateSeqStatus()
{
    if (!walletModel) return;
    interfaces::Node& node = walletModel->node();

    // Periodically re-scan the parent chain for the dual (tBTC) balance. The status
    // timer fires every 8s; refresh roughly once a minute so the slow scantxoutset
    // does not hammer the parent node (refreshBtcBalance also self-guards re-entry).
    if (++m_btc_refresh_tick % 8 == 1) refreshBtcBalance();

    // Bitcoin anchor status (node RPC)
    if (m_anchor_label) {
        try {
            UniValue r = node.executeRpc("getanchorstatus", UniValue(UniValue::VARR), std::string());
            if (r.isObject()) {
                const int tip = r.exists("tipheight") ? r["tipheight"].get_int() : -1;
                const int anc = r.exists("anchorheight") ? r["anchorheight"].get_int() : -1;
                const QString st = r.exists("anchorstatus") ? QString::fromStdString(r["anchorstatus"].get_str()) : tr("unknown");
                const bool ok = (st == QLatin1String("ok"));
                m_anchor_label->setText(tr("Bitcoin anchor: %1  -  Sequentia height %2, anchored to testnet4 block %3")
                                        .arg(ok ? tr("OK") : st).arg(tip).arg(anc));
                m_anchor_label->setStyleSheet(ok ? "color:#3ecf7a;" : "color:#ff6b6b;");
            } else {
                m_anchor_label->setText(tr("Bitcoin anchor: unavailable"));
            }
        } catch (const UniValue&) {
            m_anchor_label->setText(tr("Bitcoin anchor: unavailable"));
        } catch (const std::exception&) {
            m_anchor_label->setText(tr("Bitcoin anchor: unavailable"));
        }
    }

    // Staking / committee size + this node's producer status (from its own config)
    if (m_staking_label) {
        int n = -1;
        try {
            UniValue r = node.executeRpc("getstakerinfo", UniValue(UniValue::VARR), std::string());
            if (r.isObject()) n = (int)r.getKeys().size();
        } catch (const UniValue&) {
        } catch (const std::exception&) {}
        const bool producer = gArgs.GetBoolArg("-posproducer", false) && !gArgs.GetArgs("-posproducerkey").empty();
        // n is the number of *registered* stakers (the full registry), which is
        // distinct from and may exceed the per-block committee cap; label it as such.
        const QString registered = (n >= 0) ? tr("%1 registered staker(s)").arg(n) : tr("staker count unavailable");
        m_staking_label->setText(tr("Staking: %1 - this node is %2")
                                 .arg(registered, producer ? tr("configured to produce") : tr("not producing")));
    }

    // Finality + long-range-fork (checkpoint) status. No finalized checkpoint
    // means the chain still follows Bitcoin reorgs to any depth (the core
    // real-time-anchoring invariant); a non-empty conflicts set is a loud alarm.
    if (m_finality_label) {
        try {
            UniValue r = node.executeRpc("getcheckpointinfo", UniValue(UniValue::VARR), std::string());
            if (r.isObject()) {
                const int fin = r.exists("finalized_height") ? r["finalized_height"].get_int() : -1;
                const int depth = r.exists("depth") ? r["depth"].get_int() : 0;
                const size_t nconf = (r.exists("conflicts") && r["conflicts"].isArray()) ? r["conflicts"].size() : 0;
                if (nconf > 0) {
                    m_finality_label->setText(tr("WARNING - CHECKPOINT CONFLICT: %1 conflicting checkpoint(s) on Bitcoin. "
                                                 "A long-range fork may be in progress; do not rely on finality.").arg((int)nconf));
                    m_finality_label->setStyleSheet("QLabel{padding:6px;border-radius:4px;background:rgba(255,90,90,0.12);color:#ff6b6b;font-weight:bold;}");
                } else if (fin >= 0) {
                    m_finality_label->setText(tr("Finality: finalized up to Sequentia height %1 (checkpoint buried %2 Bitcoin blocks deep). "
                                                 "Below that height the chain still follows Bitcoin reorgs.").arg(fin).arg(depth));
                    m_finality_label->setStyleSheet("color:#3ecf7a;");
                } else {
                    m_finality_label->setText(tr("Finality: no checkpoint finalized yet - the chain follows Bitcoin reorgs to any depth. "
                                                 "A checkpoint finalizes once buried %1 Bitcoin blocks deep.").arg(depth));
                    m_finality_label->setStyleSheet("color:#ffb84d;");
                }
            } else {
                m_finality_label->setText(tr("Finality: unavailable"));
            }
        } catch (const UniValue&) {
            m_finality_label->setText(tr("Finality: unavailable"));
        } catch (const std::exception&) {
            m_finality_label->setText(tr("Finality: unavailable"));
        }
    }
}

void OverviewPage::refreshBtcBalance()
{
    if (!walletModel || !m_btc_label) return;
    // The parent-chain scantxoutset is slow (a few seconds); never run two at once.
    if (m_btc_scan_inflight) return;
    m_btc_scan_inflight = true;
    if (m_btc_label->text() == tr("Bitcoin (testnet4): loading...")) {
        m_btc_label->setText(tr("Bitcoin (testnet4): scanning the parent chain..."));
    }
    const std::string uri = "/wallet/" + walletModel->getWalletName().toStdString();
    interfaces::Node* nodePtr = &walletModel->node();
    QPointer<OverviewPage> self(this);
    // Run the (slow) parent-chain scantxoutset off the GUI thread, then post the
    // result back to the GUI thread. Avoids freezing the UI; no extra Qt module.
    std::thread([self, nodePtr, uri]() {
        QString text;
        try {
            UniValue r = nodePtr->executeRpc("getbtcbalance", UniValue(UniValue::VARR), uri);
            if (r.isObject()) {
                const std::string e = r.exists("error") ? r["error"].getValStr() : std::string();
                if (!e.empty()) {
                    text = QStringLiteral("Bitcoin (testnet4): ") + QString::fromStdString(e);
                } else {
                    const QString amt = r.exists("btc") ? QString::fromStdString(r["btc"].getValStr()) : QStringLiteral("0");
                    const int naddr = r.exists("addresses") ? r["addresses"].get_int() : 0;
                    text = QStringLiteral("Bitcoin (testnet4): ") + amt + QStringLiteral(" tBTC across ")
                           + QString::number(naddr) + QStringLiteral(" of your addresses");
                }
            } else {
                text = QStringLiteral("Bitcoin (testnet4): unexpected response");
            }
        } catch (const UniValue&) {
            text = QStringLiteral("Bitcoin (testnet4): query failed");
        } catch (const std::exception& ex) {
            text = QStringLiteral("Bitcoin (testnet4): ") + QString::fromStdString(ex.what());
        } catch (...) {
            text = QStringLiteral("Bitcoin (testnet4): query failed");
        }
        QMetaObject::invokeMethod(qApp, [self, text]() {
            if (self) {
                if (self->m_btc_label) self->m_btc_label->setText(text);
                self->m_btc_scan_inflight = false;
            }
        });
    }).detach();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if (m_balances.balance[::policyAsset] != -1) {
            setBalance(m_balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(bool use_embedded_font)
{
    QFont f = GUIUtil::fixedPitchFont(use_embedded_font);
    f.setWeight(QFont::Bold);
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
    ui->labelWatchAvailable->setFont(f);
    ui->labelWatchPending->setFont(f);
    ui->labelWatchImmature->setFont(f);
    ui->labelWatchTotal->setFont(f);
}
