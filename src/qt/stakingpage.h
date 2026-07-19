// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <QWidget>

#include <univalue.h>

#include <set>
#include <string>
#include <vector>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QShowEvent;
class QPaintEvent;
QT_END_NAMESPACE

/** A thin horizontal bar filled to `share` (0..1): this node's slice of the
 *  network's total stake. Painted rather than styled because QProgressBar
 *  cannot show a fraction of a percent, which is the interesting case here. */
class StakeShareBar : public QWidget
{
    Q_OBJECT
public:
    explicit StakeShareBar(QWidget* parent = nullptr);
    void setShare(double share);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double m_share{0.0};
};

/** One tick per recent block, highlighted where this node produced it: the
 *  shape of "how often do I actually produce" at a glance. */
class BlockStripe : public QWidget
{
    Q_OBJECT
public:
    explicit BlockStripe(QWidget* parent = nullptr);
    //! `mine[i] == true` marks the i-th block (oldest first) as produced here.
    void setBlocks(const std::vector<bool>& mine);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<bool> m_mine;
};

/**
 * Sequentia "Staking" page: one-click staking. Locks SEQ into a staking output
 * (via the registerstake wallet RPC) and shows the committee/registry status and
 * how to enable block production. All actions go through the node RPCs.
 */
class StakingPage : public QWidget
{
    Q_OBJECT

public:
    explicit StakingPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    void setModel(WalletModel* model);

public Q_SLOTS:
    void refresh();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onStake();
    void onEnableProduction();
    void onRefreshClicked();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QLabel* m_producer_status{nullptr};
    QPushButton* m_enable_button{nullptr};
    QLabel* m_summary{nullptr};
    QTableWidget* m_stakers{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QLineEdit* m_stake_amount{nullptr};
    QPushButton* m_stake_button{nullptr};
    QLabel* m_result{nullptr};
    QLabel* m_status{nullptr};

    // --- "Your stake" card ---
    QLabel* m_my_stake{nullptr};
    QLabel* m_my_share{nullptr};
    StakeShareBar* m_share_bar{nullptr};
    QLabel* m_next_slot{nullptr};
    // --- "Block production" card ---
    QLabel* m_produced_count{nullptr};
    BlockStripe* m_stripe{nullptr};
    QLabel* m_produced_fees{nullptr};
    QLabel* m_last_produced{nullptr};
    // --- watch-only key ---
    QLineEdit* m_xpub{nullptr};
    QPushButton* m_xpub_copy{nullptr};
    QLabel* m_xpub_hint{nullptr};
    // --- blocks produced by this node ---
    QLabel* m_blocks_summary{nullptr};
    QTableWidget* m_blocks{nullptr};

    //! Public keys of the stakes this wallet controls, for "is this block ours".
    std::set<std::string> m_my_pubkeys;

    //! Refresh the cards fed by getposslot / getposrecentblocks. Split out of
    //! refresh() because they read the chain rather than the wallet.
    void refreshOwnStake(const UniValue& registry);
    void refreshProducedBlocks();
    void refreshWatchOnlyKey();

    //! Run an RPC (wallet=true uses the /wallet/<name> endpoint; false the node endpoint).
    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error, bool wallet = true);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
    //! Enable autonomous block production at runtime for the given staking WIF(s)
    //! (via startposproducer). No restart. Returns true if the node is now producing.
    bool enableProduction(const QStringList& wifs, QString& err);
    //! Export WIFs for every registered stake this wallet controls (best-effort;
    //! legacy wallets only, like dumpprivkey).
    QStringList walletStakingWifs();
};

#endif // BITCOIN_QT_STAKINGPAGE_H
