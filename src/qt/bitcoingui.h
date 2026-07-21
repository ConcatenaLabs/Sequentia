// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BITCOINGUI_H
#define BITCOIN_QT_BITCOINGUI_H

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/guiutil.h>
#include <qt/optionsdialog.h>

#include <consensus/amount.h>

#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QPoint>
#include <QSystemTrayIcon>

#ifdef Q_OS_MAC
#include <qt/macos_appnap.h>
#endif

#include <memory>

class ClientModel;
class NetworkStyle;
class Notificator;
class OptionsModel;
class PlatformStyle;
class RPCConsole;
class SendCoinsRecipient;
class ReferenceCurrencyStatusBarControl;
class WalletController;
class WalletFrame;
class WalletModel;
class HelpMessageDialog;
class ModalOverlay;
enum class SynchronizationState;

namespace interfaces {
class Handler;
class Node;
struct BlockAndHeaderTipInfo;
}

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QDateTime;
class QProcess;
class QProgressBar;
class QProgressDialog;
QT_END_NAMESPACE

namespace GUIUtil {
class ClickableLabel;
class ClickableProgressBar;
}

/**
  Bitcoin GUI main class. This class represents the main window of the Bitcoin UI. It communicates with both the client and
  wallet models to give the user an up-to-date view of the current core state.
*/
class BitcoinGUI : public QMainWindow
{
    Q_OBJECT

public:
    static const std::string DEFAULT_UIPLATFORM;

    explicit BitcoinGUI(interfaces::Node& node, const PlatformStyle *platformStyle, const NetworkStyle *networkStyle, QWidget *parent = nullptr);
    ~BitcoinGUI();

    /** Set the client model.
        The client model represents the part of the core that communicates with the P2P network, and is wallet-agnostic.
    */
    void setClientModel(ClientModel *clientModel = nullptr, interfaces::BlockAndHeaderTipInfo* tip_info = nullptr);
#ifdef ENABLE_WALLET
    void setWalletController(WalletController* wallet_controller);
    WalletController* getWalletController();
#endif

#ifdef ENABLE_WALLET
    /** Set the wallet model.
        The wallet model represents a bitcoin wallet, and offers access to the list of transactions, address book and sending
        functionality.
    */
    void addWallet(WalletModel* walletModel);
    void removeWallet(WalletModel* walletModel);
    void removeAllWallets();
#endif // ENABLE_WALLET
    bool enableWallet = false;

    /** Get the tray icon status.
        Some systems have not "system tray" or "notification area" available.
    */
    bool hasTrayIcon() const { return trayIcon; }

    /** Disconnect core signals from GUI client */
    void unsubscribeFromCoreSignals();

    bool isPrivacyModeActivated() const;

protected:
    bool event(QEvent *e) override;
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    interfaces::Node& m_node;
    WalletController* m_wallet_controller{nullptr};
    std::unique_ptr<interfaces::Handler> m_handler_message_box;
    std::unique_ptr<interfaces::Handler> m_handler_question;
    ClientModel* clientModel = nullptr;
    WalletFrame* walletFrame = nullptr;

    ReferenceCurrencyStatusBarControl* refCurrencyControl = nullptr;
    GUIUtil::ThemedLabel* labelWalletEncryptionIcon = nullptr;
    GUIUtil::ThemedLabel* labelWalletHDStatusIcon = nullptr;
    GUIUtil::ClickableLabel* labelProxyIcon = nullptr;
    GUIUtil::ClickableLabel* connectionsControl = nullptr;
    GUIUtil::ClickableLabel* labelBlocksIcon = nullptr;
    QLabel* progressBarLabel = nullptr;
    GUIUtil::ClickableProgressBar* progressBar = nullptr;
    QProgressDialog* progressDialog = nullptr;
    //! True while a modal popup ("Loading wallet…", a rescan, a message box)
    //! blocks this window. Qt reports this for every modal dialog, whoever
    //! created it, which is why the sync overlay keys off it rather than off
    //! the progress signals (WalletControllerActivity builds its own dialog and
    //! emits none of them).
    bool m_window_blocked = false;
    //! The sync overlay wanted to appear while the window was blocked; show it
    //! as soon as the popup goes away.
    bool m_overlay_deferred = false;
    //! Show the sync overlay unless a modal popup owns the screen, in which case
    //! remember to show it once that popup closes.
    void showModalOverlayWhenUnblocked();

    QMenuBar* appMenuBar = nullptr;
    QToolBar* appToolBar = nullptr;
    QAction* overviewAction = nullptr;
    QAction* historyAction = nullptr;
    QAction* quitAction = nullptr;
    QAction* sendCoinsAction = nullptr;
    QAction* usedSendingAddressesAction = nullptr;
    QAction* usedReceivingAddressesAction = nullptr;
    QAction* signMessageAction = nullptr;
    QAction* verifyMessageAction = nullptr;
    QAction* m_load_psbt_action = nullptr;
    QAction* m_load_psbt_clipboard_action = nullptr;
    QAction* aboutAction = nullptr;
    QAction* receiveCoinsAction = nullptr;
    QAction* assetsAction = nullptr;
    QAction* stakingAction = nullptr;
    QAction* feePolicyAction = nullptr;
    QAction* priceServerAction = nullptr;
    QProcess* m_price_server = nullptr; // SEQUENTIA: tracked price-server sidecar (killed on exit)
    QAction* optionsAction = nullptr;
    QAction* encryptWalletAction = nullptr;
    QAction* backupWalletAction = nullptr;
    QAction* changePassphraseAction = nullptr;
    QAction* aboutQtAction = nullptr;
    QAction* openRPCConsoleAction = nullptr;
    QAction* openAction = nullptr;
    QAction* showHelpMessageAction = nullptr;
    QAction* m_create_wallet_action{nullptr};
    QAction* m_open_wallet_action{nullptr};
    QMenu* m_open_wallet_menu{nullptr};
    QAction* m_close_wallet_action{nullptr};
    QAction* m_close_all_wallets_action{nullptr};
    QAction* m_wallet_selector_label_action = nullptr;
    QAction* m_wallet_selector_action = nullptr;
    QAction* m_mask_values_action{nullptr};

    QLabel *m_wallet_selector_label = nullptr;
    QComboBox* m_wallet_selector = nullptr;

    QSystemTrayIcon* trayIcon = nullptr;
    const std::unique_ptr<QMenu> trayIconMenu;
    Notificator* notificator = nullptr;
    RPCConsole* rpcConsole = nullptr;
    HelpMessageDialog* helpMessageDialog = nullptr;
    ModalOverlay* modalOverlay = nullptr;

    QMenu* m_network_context_menu = new QMenu(this);

#ifdef Q_OS_MAC
    CAppNapInhibitor* m_app_nap_inhibitor = nullptr;
#endif

    /** Keep track of previous number of blocks, to detect progress */
    int prevBlocks = 0;
    int spinnerFrame = 0;

    /** SEQUENTIA "waiting for the Bitcoin network" stall notice (incident
        2026-07-11 §8.3). The timer only queries the node while the tip is
        stalled; m_last_tip_advance is when the tip last moved (wall clock,
        seeded from the tip block's own timestamp at startup). */
    QTimer* m_anchor_wait_timer = nullptr;
    int64_t m_last_tip_advance = 0;
    bool m_anchor_wait_active = false;

    const PlatformStyle *platformStyle;
    const NetworkStyle* const m_network_style;

    /** Create the main UI actions. */
    void createActions();
    /** Create the menu bar and sub-menus. */
    void createMenuBar();
    /** Create the toolbars */
    void createToolBars();
    /** Create system tray icon and notification */
    void createTrayIcon();
    /** Create system tray menu (or setup the dock menu) */
    void createTrayIconMenu();

    /** Enable or disable all wallet-related actions */
    void setWalletActionsEnabled(bool enabled);

    /** Connect core signals to GUI client */
    void subscribeToCoreSignals();

    /** Update UI with latest network info from model. */
    void updateNetworkState();

    void updateHeadersSyncProgressLabel();

    /** SEQUENTIA: show (or retire) the status-bar notice that the chain has
        paused because of Bitcoin — fork being settled, or Bitcoin daemon
        unreachable. Runs on m_anchor_wait_timer. */
    void updateAnchorWaitStatus();

    /** Open the OptionsDialog on the specified tab index */
    void openOptionsDialogWithTab(OptionsDialog::Tab tab);

Q_SIGNALS:
    void quitRequested();
    /** Signal raised when a URI was entered or dragged to the GUI */
    void receivedURI(const QString &uri);
    /** Signal raised when RPC console shown */
    void consoleShown(RPCConsole* console);
    void setPrivacy(bool privacy);

public Q_SLOTS:
    /** Set number of connections shown in the UI */
    void setNumConnections(int count);
    /** Set network state shown in the UI */
    void setNetworkActive(bool network_active);
    /** Set number of blocks and last block date shown in the UI */
    void setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers, SynchronizationState sync_state);

    /** Notify the user of an event from the core network or transaction handling code.
       @param[in] title             the message box / notification title
       @param[in] message           the displayed text
       @param[in] style             modality and style definitions (icon and used buttons - buttons only for message boxes)
                                    @see CClientUIInterface::MessageBoxFlags
       @param[in] ret               pointer to a bool that will be modified to whether Ok was clicked (modal only)
       @param[in] detailed_message  the text to be displayed in the details area
    */
    void message(const QString& title, QString message, unsigned int style, bool* ret = nullptr, const QString& detailed_message = QString());

#ifdef ENABLE_WALLET
    void setCurrentWallet(WalletModel* wallet_model);
    void setCurrentWalletBySelectorIndex(int index);
    /** Set the UI status indicators based on the currently selected wallet.
    */
    void updateWalletStatus();

private:
    /** Set the encryption status as shown in the UI.
       @param[in] status            current encryption status
       @see WalletModel::EncryptionStatus
    */
    void setEncryptionStatus(int status);

    /** Set the hd-enabled status as shown in the UI.
     @param[in] hdEnabled         current hd enabled status
     @see WalletModel::EncryptionStatus
     */
    void setHDStatus(bool privkeyDisabled, int hdEnabled);

public Q_SLOTS:
    bool handlePaymentRequest(const SendCoinsRecipient& recipient);

    /** Show incoming transaction notification for new transactions. */
    void incomingTransaction(const QString& date, const QString& assetamount_str, const QString& type, const QString& address, const QString& label, const QString& walletName);
#endif // ENABLE_WALLET

private:
    /** Set the proxy-enabled icon as shown in the UI. */
    void updateProxyIcon();
    void updateWindowTitle();

public Q_SLOTS:
#ifdef ENABLE_WALLET
    /** Switch to overview (home) page */
    void gotoOverviewPage();
    /** Switch to history (transactions) page */
    void gotoHistoryPage();
    /** Switch to receive coins page */
    void gotoReceiveCoinsPage();
    /** Switch to the Assets page */
    void gotoAssetsPage();
    /** Switch to the Staking page */
    void gotoStakingPage();
    /** Open the fee-acceptance policy dialog */
    void gotoFeePolicyDialog();
    /** Launch the bundled price-server sidecar and open its configuration page */
    void launchPriceServer();
    /** Terminate the price-server sidecar if running (called on GUI teardown). */
    void stopPriceServer();
    /** Switch to send coins page */
    void gotoSendCoinsPage(QString addr = "");

    /** Show Sign/Verify Message dialog and switch to sign message tab */
    void gotoSignMessageTab(QString addr = "");
    /** Show Sign/Verify Message dialog and switch to verify message tab */
    void gotoVerifyMessageTab(QString addr = "");
    /** Load Partially Signed Bitcoin Transaction from file or clipboard */
    void gotoLoadPSBT(bool from_clipboard = false);

#endif // ENABLE_WALLET
    /** Show configuration dialog */
    void optionsClicked();
    /** Show about dialog */
    void aboutClicked();
    /** Show debug window */
    void showDebugWindow();
    /** Show debug window and set focus to the console */
    void showDebugWindowActivateConsole();
    /** Show help message dialog */
    void showHelpMessageClicked();

    /** Show window if hidden, unminimize when minimized, rise when obscured or show if hidden and fToggleHidden is true */
    void showNormalIfMinimized() { showNormalIfMinimized(false); }
    void showNormalIfMinimized(bool fToggleHidden);
    /** Simply calls showNormalIfMinimized(true) for use in SLOT() macro */
    void toggleHidden();

    /** called by a timer to check if ShutdownRequested() has been set **/
    void detectShutdown();

    /** Show progress dialog e.g. for verifychain */
    void showProgress(const QString &title, int nProgress);

    void showModalOverlay();
};

/** SEQUENTIA: status-bar control to pick the reference currency used for the "≈" valuation
 *  shown beside displayed amounts (USD, BTC, or any priced asset). A QComboBox (not a
 *  QLabel+QMenu) so its popup grabs the mouse reliably under Xwayland/xcb. */
class ReferenceCurrencyStatusBarControl : public QComboBox
{
    Q_OBJECT

public:
    explicit ReferenceCurrencyStatusBarControl(const PlatformStyle *platformStyle);
    /** Lets the control know about the Options Model (and its signals) */
    void setOptionsModel(OptionsModel *optionsModel);

protected:
    /** Refresh the option list (late-arriving prices) just before the popup opens. */
    void showPopup() override;

private:
    OptionsModel *optionsModel;
    const PlatformStyle* m_platform_style;
    bool m_updating{false};

    /** (Re)builds the list from USD + BTC + every currently-priced asset ticker. */
    void rebuild();

private Q_SLOTS:
    /** Keep the selection in sync when the reference currency changes in the model. */
    void updateReferenceCurrency(const QString& ticker);
    /** Tells the underlying optionsModel to update its current reference currency. */
    void onActivated(int index);
};

#endif // BITCOIN_QT_BITCOINGUI_H
