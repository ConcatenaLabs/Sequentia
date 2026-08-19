// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STAKINGPAGE_H
#define BITCOIN_QT_STAKINGPAGE_H

#include <QWidget>

#include <univalue.h>

#include <map>
#include <set>
#include <string>
#include <vector>

class WalletModel;
class PlatformStyle;

QT_BEGIN_NAMESPACE
class QComboBox;
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
    void onUnstake();
    //! Put the largest usable amount into the respective Amount field.
    void onStakeMax();
    void onUnstakeMax();
    //! Re-send the pending withdrawal with a higher network fee.
    void onUnstakeBump();
    void onEnableProduction();
    void onRefreshClicked();
    //! Lend this wallet's stake weight to the signer named in the field, or
    //! take it back. Neither moves the staked coins.
    void onDelegate();
    void onUndelegate();
    //! Operator side: commit on-chain to how this node's blocks pay out.
    //! Running a pool is a node operation and lives only here.
    void onAnnouncePayout();
    void onPayoutPresetChanged();

private:
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;

    QLabel* m_producer_status{nullptr};
    QPushButton* m_enable_button{nullptr};
    QLabel* m_summary{nullptr};
    QTableWidget* m_stakers{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QLineEdit* m_stake_amount{nullptr};
    QPushButton* m_stake_max{nullptr};    //!< fill Amount with everything stakeable (balance minus fee headroom)
    QPushButton* m_stake_button{nullptr};
    QLabel* m_result{nullptr};
    QLabel* m_status{nullptr};

    // --- "Withdraw stake" (unstake) card ---
    QLabel* m_unstake_info{nullptr};      //!< what is withdrawable now / still unbonding
    QLineEdit* m_unstake_amount{nullptr};
    QPushButton* m_unstake_max{nullptr};  //!< fill Amount with everything withdrawable
    QPushButton* m_unstake_button{nullptr};
    //! Shown only while a withdrawal is waiting to confirm: re-sends it with a
    //! higher fee. The wallet's own RBF cannot touch these transactions.
    QPushButton* m_unstake_bump{nullptr};
    //! Enabled wrapper around the Withdraw button: carries the tooltip that says
    //! why the button is greyed out (a disabled widget gets no tooltip events).
    QWidget* m_unstake_button_holder{nullptr};
    QLabel* m_unstake_result{nullptr};

    // --- "Staking pool" card: delegate to a pool, or watch the one you are in ---
    //! The delegator's watch. A pool must announce a payout-policy change a
    //! notice period before it binds -- which only protects a delegator who
    //! SEES the notice, so anything pending is shouted here rather than filed
    //! in a table.
    QLabel* m_deleg_alerts{nullptr};
    QLabel* m_deleg_status{nullptr};
    QLineEdit* m_deleg_amount{nullptr};
    QLineEdit* m_deleg_signer{nullptr};
    QPushButton* m_deleg_button{nullptr};
    QPushButton* m_undeleg_button{nullptr};
    QLabel* m_deleg_result{nullptr};

    // --- "Run a staking pool" card: the OPERATOR console ---
    //! Deliberately node-only. Announcing a payout policy binds every block this
    //! key produces, needs the signer's own key, and is audited by strangers, so
    //! it belongs with the machine that produces the blocks rather than in a
    //! phone or browser wallet that cannot even be online when a block is due.
    QLabel* m_pool_status{nullptr};
    QLabel* m_pool_commitment{nullptr};
    //! Common payout policies, so an operator picks a shape rather than
    //! inventing a commission from nothing. "Custom" reveals the raw fields.
    QComboBox* m_payout_preset{nullptr};
    QLabel* m_payout_preset_note{nullptr};
    //! Truly custom: the exact scriptPubKey every coinbase must pay, and the
    //! height it binds from. Nothing else in the card can express these.
    QLineEdit* m_payout_script{nullptr};
    QLabel* m_payout_script_label{nullptr};
    QLineEdit* m_payout_activation{nullptr};
    QLabel* m_payout_activation_label{nullptr};
    //! Which of this wallet's staker keys the policy binds. Only shown when the
    //! wallet stakes with more than one: announcing is per-key, and with several
    //! the RPC cannot guess, so without this the card would be a dead end for
    //! exactly the operator who keeps a vesting-locked tranche and a small
    //! freely-spendable stake side by side.
    QComboBox* m_payout_signer{nullptr};
    QLabel* m_payout_signer_label{nullptr};
    QLineEdit* m_payout_commission{nullptr};
    QLineEdit* m_payout_address{nullptr};
    QLabel* m_payout_commission_label{nullptr};
    QLabel* m_payout_address_label{nullptr};
    QPushButton* m_payout_button{nullptr};
    QLabel* m_payout_result{nullptr};

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
    //! Stake weight the REGISTRY credits to this wallet's keys (atoms). Can be
    //! non-zero while the wallet holds no withdrawable staking output — config
    //! declared stake (-staker / genesis) has no UTXO at all, and an output
    //! funded by another wallet is not in this one's history. The Withdraw card
    //! needs it to explain that gap instead of claiming nothing is staked.
    uint64_t m_registry_stake{0};
    //! Cache of the costly per-staker ownership check (3 RPCs each): pubkey ->
    //! does this wallet control it. Membership never changes for a given key, so
    //! we only pay the derivation the first time we see a registered pubkey.
    std::map<std::string, bool> m_ismine_cache;

    //! Refresh the cards fed by getposslot / getposrecentblocks. Split out of
    //! refresh() because they read the chain rather than the wallet.
    void refreshOwnStake(const UniValue& registry);
    void refreshProducedBlocks();
    void refreshWatchOnlyKey();
    //! Refresh the "Withdraw stake" card from liststakeutxos: how much is
    //! withdrawable right now, how much is still unbonding and until when.
    //! Pass an already-fetched list to render exactly those numbers, so an
    //! action and the summary above it can never quote two different totals.
    void refreshUnstakeInfo(const UniValue* prefetched = nullptr);
    //! Refresh the "Staking pool" card: where this wallet's weight is signing,
    //! what that pool has committed to, what it has announced but not yet bound
    //! (listdelegations), and the board of pools to choose from (listpools).
    void refreshDelegation();
    //! Refresh the "Run a staking pool" card: what this node's signer commands,
    //! who lent it, how reliably it produces, and what it has committed to.
    void refreshPoolOperator();
    //! The policy the card is about to announce, read from the chosen entry.
    bool payoutModeIsLottery() const;
    int64_t payoutCommissionBp() const;

    //! Run an RPC (wallet=true uses the /wallet/<name> endpoint; false the node endpoint).
    UniValue callRpc(const std::string& method, const UniValue& params, bool& ok, QString& error, bool wallet = true);
    std::string walletUri() const;
    void setStatus(const QString& msg, bool error = false);
    //! Report the outcome of a card's action IN that card, next to the button
    //! that caused it. The page-wide status line sits below every card, off the
    //! bottom of a scrolled page, so a refusal shown only there reads as "the
    //! button does nothing".
    void setCardResult(QLabel* result, const QString& msg, bool error);
    //! Enable autonomous block production at runtime for the given staking WIF(s)
    //! (via startposproducer). No restart. Returns true if the node is now producing.
    bool enableProduction(const QStringList& wifs, QString& err);
    //! Export WIFs for every registered stake this wallet controls (best-effort;
    //! legacy wallets only, like dumpprivkey).
    QStringList walletStakingWifs();

    //! Kick a refresh onto the next event-loop turn so the tab switch paints
    //! first, never blocking the switch on the registry/chain RPC cascade. When
    //! force is false it also skips a re-run that would just redo the current
    //! result (same tip, refreshed a moment ago).
    void scheduleRefresh(bool force);
    bool m_refresh_pending{false};   //!< a deferred refresh is already queued
    int m_last_refresh_blocks{-1};   //!< tip height at the last completed refresh
    qint64 m_last_refresh_ms{0};     //!< wall-clock ms of the last completed refresh
};

#endif // BITCOIN_QT_STAKINGPAGE_H
