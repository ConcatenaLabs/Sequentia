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
#include <QColor>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStatusTipEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <vector>

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

namespace {
// Columns of the per-asset balances table. Pending/Immature are hidden when no asset has
// any such amount, so the common case reads as a clean Asset | Available | Value table.
enum AssetCol { COL_ASSET = 0, COL_ID, COL_AVAILABLE, COL_PENDING, COL_IMMATURE, COL_VALUE, COL_COUNT };
} // namespace

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

    // Sequentia: the Overview no longer duplicates a "Recent transactions" list — the
    // dedicated Transactions tab now carries the full, filterable history (with fees as
    // sub-entries). Hide the whole panel so the Balances panel owns the page. The list
    // machinery above is left wired (harmless, unshown) to keep the transactionClicked
    // signal other views connect to intact.
    ui->frame_2->setVisible(false);

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
        m_headline_point_size = f.pointSizeF() * 1.9;
        f.setPointSizeF(m_headline_point_size);
        f.setBold(true);
        m_total_value->setFont(f);
        m_total_value->setVisible(false);
        // Row 1 of the panel: under the "Balances" title, above the amounts.
        ui->verticalLayout_4->insertWidget(1, m_total_value);
    }

    // Sequentia: replace the old amount+id label grid (where a token quantity and a 64-hex
    // asset id ran together with no headers — you could not tell which was which) with a
    // proper labelled table: Asset | Available | Pending | Immature | Value. The scroll-area
    // grid is retired; the table carries its own scrollbar when there are many assets.
    {
        ui->scrollArea->setVisible(false);

        m_asset_table = new QTableWidget(0, COL_COUNT, ui->frame);
        m_asset_table->setHorizontalHeaderLabels({tr("Asset name"), tr("Asset id"), tr("Available"), tr("Pending"), tr("Immature"), tr("Value")});
        // Name is short; the id gets the stretch and elides with "…" when narrowed
        // (the full id is always on hover), so the two are never confused with the
        // amount and each has its own resizable column.
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_ASSET, QHeaderView::ResizeToContents);
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_ID, QHeaderView::Stretch);
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_AVAILABLE, QHeaderView::ResizeToContents);
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_PENDING, QHeaderView::ResizeToContents);
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_IMMATURE, QHeaderView::ResizeToContents);
        m_asset_table->horizontalHeader()->setSectionResizeMode(COL_VALUE, QHeaderView::ResizeToContents);
        m_asset_table->horizontalHeader()->setHighlightSections(false);
        m_asset_table->setTextElideMode(Qt::ElideRight);
        m_asset_table->verticalHeader()->setVisible(false);
        m_asset_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_asset_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_asset_table->setSelectionMode(QAbstractItemView::NoSelection);
        m_asset_table->setFocusPolicy(Qt::NoFocus);
        m_asset_table->setShowGrid(false);
        m_asset_table->setWordWrap(false);
        m_asset_table->setAlternatingRowColors(true);
        m_asset_table->setMinimumHeight(120);
        // Insert where the retired scroll area sat: under the total-value headline, above the
        // tBTC separator/label that setBalance appends below the asset rows.
        ui->verticalLayout_4->insertWidget(2, m_asset_table);
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

    const QString refCur = walletModel->getOptionsModel()->getReferenceCurrency();

    // The headline: everything spendable plus everything still confirming, valued
    // in one number. Hidden in privacy mode (it would unmask the masked rows).
    if (m_total_value) {
        const CAmountMap all = walletModel->wallet().privateKeysDisabled()
            ? balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance
            : balances.balance + balances.unconfirmed_balance + balances.immature_balance;
        bool holds_anything = false;
        for (const auto& it : all) if (it.second > 0) { holds_anything = true; break; }

        QString text;
        QFont f = m_total_value->font();
        if (!holds_anything) {
            // An empty wallet holds no assets — not "0 of the native one". Say that
            // plainly instead of leaving the rows to imply it with dashes.
            text = tr("No assets yet");
            f.setPointSizeF(m_headline_point_size * 0.62);
            m_total_value->setStyleSheet("color:#9b988e;");
            m_total_value->setToolTip(tr("Anything you receive shows up here, each asset on its own line. "
                                         "Use the Receive tab to get an address."));
        } else {
            text = GUIUtil::formatMultiAssetReferenceApprox(all, refCur);
            f.setPointSizeF(m_headline_point_size);
            m_total_value->setStyleSheet(QString());
            m_total_value->setToolTip(tr("Everything in this wallet, valued at the latest prices the node has. "
                                         "Assets with no published price are not counted."));
        }
        m_total_value->setFont(f);
        m_total_value->setText(m_privacy ? QString() : text);
        m_total_value->setVisible(!m_privacy && !text.isEmpty());
    }

    populateAssetTable(balances, unit, refCur);
}

void OverviewPage::populateAssetTable(const interfaces::WalletBalances& balances, int unit, const QString& refCur)
{
    if (!m_asset_table) return;

    QTableWidget* t = m_asset_table;
    t->setRowCount(0);

    bool anyPending = false;
    bool anyImmature = false;

    // Amount in map `m` for asset `a`, 0 if absent.
    auto get = [](const CAmountMap& m, const CAsset& a) -> CAmount {
        auto it = m.find(a);
        return it != m.end() ? it->second : 0;
    };
    // A right-aligned amount cell, blank for zero, masked in privacy mode. The asset name is
    // in the Asset column, so the number itself is rendered without any asset suffix.
    auto amountCell = [&](const CAsset& asset, CAmount v) -> QTableWidgetItem* {
        QString s;
        if (m_privacy && v > 0) s = QString::fromUtf8("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"); // "••••"
        else if (v > 0)         s = GUIUtil::formatAssetAmount(asset, v, unit, BitcoinUnits::SeparatorStyle::ALWAYS, false);
        auto* item = new QTableWidgetItem(s);
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    };
    auto addRow = [&](const CAsset& asset, CAmount avail, CAmount pending, CAmount immature, const QString& suffix) {
        const CAmount total = avail + pending + immature;
        if (total <= 0) return;
        const int row = t->rowCount();
        t->insertRow(row);

        // Asset name (registry) and asset id (the 64-hex identity) now live in two
        // separate columns. When the registry has not named the asset, the name cell
        // says "N/A" and explains in a tooltip why; the id is always shown in full,
        // eliding with "…" only when the column is dragged narrow.
        const bool named = GUIUtil::assetIsNamed(asset);
        const QString fullId = QString::fromStdString(asset.GetHex());
        const QString name = GUIUtil::assetDisplayName(asset);

        auto* a0 = new QTableWidgetItem((named ? name : tr("N/A")) + suffix);
        if (named) {
            a0->setToolTip(name);
        } else {
            a0->setForeground(QColor("#9b988e"));
            a0->setToolTip(tr("No registered name yet. An asset's name comes from the asset registry once "
                              "its issuer publishes the proof file on their domain and registers it; until "
                              "then wallets can only show its id."));
        }
        t->setItem(row, COL_ASSET, a0);

        auto* aid = new QTableWidgetItem(fullId);
        aid->setToolTip(fullId);
        t->setItem(row, COL_ID, aid);

        t->setItem(row, COL_AVAILABLE, amountCell(asset, avail));
        t->setItem(row, COL_PENDING, amountCell(asset, pending));
        t->setItem(row, COL_IMMATURE, amountCell(asset, immature));
        if (pending > 0) anyPending = true;
        if (immature > 0) anyImmature = true;

        // Value of everything held of this asset (available + pending + immature) in the
        // chosen reference currency. Blank ("—", muted) when the asset has no published price
        // or is itself the reference. Suppressed in privacy mode (it would unmask the amount).
        QString val;
        if (!m_privacy) val = GUIUtil::formatReferenceApprox(asset, total, refCur);
        auto* v = new QTableWidgetItem(val.isEmpty() ? QString::fromUtf8("\xE2\x80\x94") : val); // "—"
        v->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (val.isEmpty()) v->setForeground(QColor("#9b988e"));
        t->setItem(row, COL_VALUE, v);
    };
    // Add one row per asset held across the three maps, native (tSEQ/SEQ) first, then the rest
    // in id order. `suffix` marks watch-only rows.
    auto addSection = [&](const CAmountMap& avail, const CAmountMap& pending, const CAmountMap& immature, const QString& suffix) {
        std::vector<CAsset> order;
        std::set<CAsset> seen;
        auto consider = [&](const CAsset& a) { if (seen.insert(a).second) order.push_back(a); };
        if (get(avail, ::policyAsset) + get(pending, ::policyAsset) + get(immature, ::policyAsset) > 0) consider(::policyAsset);
        for (const auto& it : avail)    if (it.second > 0) consider(it.first);
        for (const auto& it : pending)  if (it.second > 0) consider(it.first);
        for (const auto& it : immature) if (it.second > 0) consider(it.first);
        for (const CAsset& a : order) addRow(a, get(avail, a), get(pending, a), get(immature, a), suffix);
    };

    if (walletModel->wallet().privateKeysDisabled()) {
        // Watch-only wallet: the "watch-only" maps are the only balances it has.
        addSection(balances.watch_only_balance, balances.unconfirmed_watch_only_balance, balances.immature_watch_only_balance, QString());
    } else {
        addSection(balances.balance, balances.unconfirmed_balance, balances.immature_balance, QString());
        // Any watch-only holdings follow, tagged so they are not confused with spendable ones.
        addSection(balances.watch_only_balance, balances.unconfirmed_watch_only_balance, balances.immature_watch_only_balance, tr(" (watch-only)"));
    }

    // Hide the Pending/Immature columns entirely when nothing needs them, so the everyday case
    // reads as a clean Asset | Available | Value table.
    t->setColumnHidden(COL_PENDING, !anyPending);
    t->setColumnHidden(COL_IMMATURE, !anyImmature);
    // With no holdings the "No assets yet" headline says it; an empty header-only grid would
    // just be noise, so hide the table until there is at least one row.
    t->setVisible(t->rowCount() > 0);
    t->resizeRowsToContents();

    // Remember what we just drew, so the status-timer refresh can tell whether a later
    // registry/price update would actually change anything before rebuilding.
    m_asset_sig = assetTableSignature(balances, refCur);
}

QString OverviewPage::assetTableSignature(const interfaces::WalletBalances& balances, const QString& refCur) const
{
    auto get = [](const CAmountMap& m, const CAsset& a) -> CAmount {
        auto it = m.find(a);
        return it != m.end() ? it->second : 0;
    };
    QString sig = refCur + (m_privacy ? QStringLiteral("|P") : QStringLiteral("|."));
    auto section = [&](const CAmountMap& av, const CAmountMap& pe, const CAmountMap& im) {
        std::set<CAsset> seen;
        auto one = [&](const CAsset& a) {
            if (!seen.insert(a).second) return;
            const CAmount tot = get(av, a) + get(pe, a) + get(im, a);
            if (tot <= 0) return;
            sig += QStringLiteral("|") + GUIUtil::assetDisplayName(a)
                 + QStringLiteral("=") + GUIUtil::formatReferenceApprox(a, tot, refCur);
        };
        for (const auto& it : av) if (it.second > 0) one(it.first);
        for (const auto& it : pe) if (it.second > 0) one(it.first);
        for (const auto& it : im) if (it.second > 0) one(it.first);
    };
    if (walletModel->wallet().privateKeysDisabled()) {
        section(balances.watch_only_balance, balances.unconfirmed_watch_only_balance, balances.immature_watch_only_balance);
    } else {
        section(balances.balance, balances.unconfirmed_balance, balances.immature_balance);
        section(balances.watch_only_balance, balances.unconfirmed_watch_only_balance, balances.immature_watch_only_balance);
    }
    return sig;
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

    // Asset names (registry) and reference prices arrive from the node's feeds a few seconds
    // after startup, and prices refresh periodically thereafter. The balances table is otherwise
    // only rebuilt on a wallet balanceChanged, so without this it would keep showing raw asset
    // ids and blank values until the next wallet event. Rebuild only when the rendered content
    // would actually change, so the steady state doesn't flicker every tick.
    if (m_asset_table && m_balances.balance[::policyAsset] != -1) {
        const QString refCur = walletModel->getOptionsModel()->getReferenceCurrency();
        if (assetTableSignature(m_balances, refCur) != m_asset_sig) setBalance(m_balances);
    }

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
                    // A genuine alarm: two checkpoints disagree on Bitcoin.
                    m_finality_label->setText(tr("WARNING - CHECKPOINT CONFLICT: %1 conflicting checkpoint(s) on Bitcoin. "
                                                 "A long-range fork may be in progress; do not rely on finality.").arg((int)nconf));
                    m_finality_label->setStyleSheet("QLabel{padding:6px;border-radius:4px;background:rgba(255,90,90,0.12);color:#ff6b6b;font-weight:bold;}");
                    m_finality_label->setVisible(true);
                } else if (fin >= 0) {
                    // Something positive to report: a checkpoint is buried and final.
                    m_finality_label->setText(tr("Finalized up to Sequentia height %1 - a checkpoint is written on Bitcoin, buried %2 blocks deep.").arg(fin).arg(depth));
                    m_finality_label->setStyleSheet("color:#3ecf7a;");
                    m_finality_label->setVisible(true);
                } else {
                    // No checkpoint yet is the ordinary state of real-time anchoring,
                    // not a problem, so say nothing rather than raise a yellow flag
                    // that reads as "your blocks are not final".
                    m_finality_label->setVisible(false);
                }
            } else {
                m_finality_label->setVisible(false);
            }
        } catch (const UniValue&) {
            m_finality_label->setVisible(false);
        } catch (const std::exception&) {
            m_finality_label->setVisible(false);
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
