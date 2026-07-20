// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitcoingui.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/createwalletdialog.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/modaloverlay.h>
#include <qt/networkstyle.h>
#include <qt/notificator.h>
#include <qt/openuridialog.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>

#include <referenceprices.h>
#include <qt/platformstyle.h>
#include <qt/rpcconsole.h>
#include <qt/utilitydialog.h>

#ifdef ENABLE_WALLET
#include <qt/walletcontroller.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#endif // ENABLE_WALLET

#ifdef Q_OS_MAC
#include <qt/macdockiconhandler.h>
#endif

#include <functional>
#include <chain.h>
#include <chainparams.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <node/ui_interface.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>

#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTcpSocket>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolBar>
#include <QButtonGroup>
#include <QPushButton>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWindow>


const std::string BitcoinGUI::DEFAULT_UIPLATFORM =
#if defined(Q_OS_MAC)
        "macosx"
#elif defined(Q_OS_WIN)
        "windows"
#else
        "other"
#endif
        ;

BitcoinGUI::BitcoinGUI(interfaces::Node& node, const PlatformStyle *_platformStyle, const NetworkStyle *networkStyle, QWidget *parent) :
    QMainWindow(parent),
    m_node(node),
    trayIconMenu{new QMenu()},
    platformStyle(_platformStyle),
    m_network_style(networkStyle)
{
    QSettings settings;
    if (!GUIUtil::RestoreWindowGeometry(this, settings.value("MainWindowGeometry").toByteArray())) {
        // Restore failed, was missing, or held an unusable geometry: center the window.
        GUIUtil::MoveToScreenCenter(this);
    }

    setContextMenuPolicy(Qt::PreventContextMenu);

#ifdef ENABLE_WALLET
    enableWallet = WalletModel::isWalletEnabled();
#endif // ENABLE_WALLET
    QApplication::setWindowIcon(m_network_style->getTrayAndWindowIcon());
    setWindowIcon(m_network_style->getTrayAndWindowIcon());
    updateWindowTitle();

    rpcConsole = new RPCConsole(node, _platformStyle, nullptr);
    helpMessageDialog = new HelpMessageDialog(this, false);
#ifdef ENABLE_WALLET
    if(enableWallet)
    {
        /** Create wallet frame and make it the central widget */
        walletFrame = new WalletFrame(_platformStyle, this);
        connect(walletFrame, &WalletFrame::createWalletButtonClicked, [this] {
            auto activity = new CreateWalletActivity(getWalletController(), this);
            activity->create();
        });
        connect(walletFrame, &WalletFrame::message, [this](const QString& title, const QString& message, unsigned int style) {
            this->message(title, message, style);
        });
        connect(walletFrame, &WalletFrame::currentWalletSet, [this] { updateWalletStatus(); });
        setCentralWidget(walletFrame);
    } else
#endif // ENABLE_WALLET
    {
        /* When compiled without wallet or -disablewallet is provided,
         * the central widget is the rpc console.
         */
        setCentralWidget(rpcConsole);
        Q_EMIT consoleShown(rpcConsole);
    }

    modalOverlay = new ModalOverlay(enableWallet, this->centralWidget());

    // Accept D&D of URIs
    setAcceptDrops(true);

    // Create actions for the toolbar, menu bar and tray/dock icon
    // Needs walletFrame to be initialized
    createActions();

    // Create application menu bar
    createMenuBar();

    // Create the toolbars
    createToolBars();

    // Create system tray icon and notification
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        createTrayIcon();
    }
    notificator = new Notificator(QApplication::applicationName(), trayIcon, this);

    // Create status bar
    statusBar();

    // Disable size grip because it looks ugly and nobody needs it
    statusBar()->setSizeGripEnabled(false);

    // Status bar notification icons
    QFrame *frameBlocks = new QFrame();
    frameBlocks->setContentsMargins(0,0,0,0);
    frameBlocks->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QHBoxLayout *frameBlocksLayout = new QHBoxLayout(frameBlocks);
    frameBlocksLayout->setContentsMargins(3,0,3,0);
    frameBlocksLayout->setSpacing(3);
    // SEQUENTIA: no native-unit (tSEQ/mtSEQ/sat) picker — amounts show in whole asset units.
    refCurrencyControl = new ReferenceCurrencyStatusBarControl(platformStyle);
    labelWalletEncryptionIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelWalletHDStatusIcon = new GUIUtil::ThemedLabel(platformStyle);
    labelProxyIcon = new GUIUtil::ClickableLabel(platformStyle);
    connectionsControl = new GUIUtil::ClickableLabel(platformStyle);
    labelBlocksIcon = new GUIUtil::ClickableLabel(platformStyle);
    if(enableWallet)
    {
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(refCurrencyControl);
        frameBlocksLayout->addStretch();
        frameBlocksLayout->addWidget(labelWalletEncryptionIcon);
        labelWalletEncryptionIcon->hide();
        frameBlocksLayout->addWidget(labelWalletHDStatusIcon);
        labelWalletHDStatusIcon->hide();
    }
    frameBlocksLayout->addWidget(labelProxyIcon);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(connectionsControl);
    frameBlocksLayout->addStretch();
    frameBlocksLayout->addWidget(labelBlocksIcon);
    frameBlocksLayout->addStretch();

    // Progress bar and label for blocks download
    progressBarLabel = new QLabel();
    progressBarLabel->setVisible(false);
    progressBar = new GUIUtil::ProgressBar();
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setVisible(false);

    // Override style sheet for progress bar for styles that have a segmented progress bar,
    // as they make the text unreadable (workaround for issue #1071)
    // See https://doc.qt.io/qt-5/gallery.html
    QString curStyle = QApplication::style()->metaObject()->className();
    if(curStyle == "QWindowsStyle" || curStyle == "QWindowsXPStyle")
    {
        progressBar->setStyleSheet("QProgressBar { background-color: #e8e8e8; border: 1px solid grey; border-radius: 7px; padding: 1px; text-align: center; } QProgressBar::chunk { background: QLinearGradient(x1: 0, y1: 0, x2: 1, y2: 0, stop: 0 #FF8000, stop: 1 orange); border-radius: 7px; margin: 0px; }");
    }

    statusBar()->addWidget(progressBarLabel);
    statusBar()->addWidget(progressBar);
    statusBar()->addPermanentWidget(frameBlocks);

    // Install event filter to be able to catch status tip events (QEvent::StatusTip)
    this->installEventFilter(this);

    // Initially wallet actions should be disabled
    setWalletActionsEnabled(false);

    // Subscribe to notifications from core
    subscribeToCoreSignals();

    connect(labelProxyIcon, &GUIUtil::ClickableLabel::clicked, [this] {
        openOptionsDialogWithTab(OptionsDialog::TAB_NETWORK);
    });

    connect(labelBlocksIcon, &GUIUtil::ClickableLabel::clicked, this, &BitcoinGUI::showModalOverlay);
    connect(progressBar, &GUIUtil::ClickableProgressBar::clicked, this, &BitcoinGUI::showModalOverlay);

#ifdef Q_OS_MAC
    m_app_nap_inhibitor = new CAppNapInhibitor;
#endif

    GUIUtil::handleCloseWindowShortcut(this);
}

BitcoinGUI::~BitcoinGUI()
{
    // SEQUENTIA: stop the price-server sidecar so it never outlives the GUI.
    stopPriceServer();

    // Unsubscribe from notifications from core
    unsubscribeFromCoreSignals();

    QSettings settings;
    settings.setValue("MainWindowGeometry", saveGeometry());
    if(trayIcon) // Hide tray icon, as deleting will let it linger until quit (on Ubuntu)
        trayIcon->hide();
#ifdef Q_OS_MAC
    delete m_app_nap_inhibitor;
    delete appMenuBar;
    MacDockIconHandler::cleanup();
#endif

    delete rpcConsole;
}

void BitcoinGUI::createActions()
{
    QActionGroup *tabGroup = new QActionGroup(this);
    connect(modalOverlay, &ModalOverlay::triggered, tabGroup, &QActionGroup::setEnabled);

    overviewAction = new QAction(platformStyle->SingleColorIcon(":/icons/overview"), tr("&Overview"), this);
    overviewAction->setStatusTip(tr("Show general overview of wallet"));
    overviewAction->setToolTip(overviewAction->statusTip());
    overviewAction->setCheckable(true);
    overviewAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_1));
    tabGroup->addAction(overviewAction);

    sendCoinsAction = new QAction(platformStyle->SingleColorIcon(":/icons/send"), tr("&Send"), this);
    sendCoinsAction->setStatusTip(tr("Send coins to a %1 address").arg("Sequentia"));
    sendCoinsAction->setToolTip(sendCoinsAction->statusTip());
    sendCoinsAction->setCheckable(true);
    sendCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_2));
    tabGroup->addAction(sendCoinsAction);

    receiveCoinsAction = new QAction(platformStyle->SingleColorIcon(":/icons/receiving_addresses"), tr("&Receive"), this);
    receiveCoinsAction->setStatusTip(tr("Request payments (generates QR codes and %1 addresses)").arg("Sequentia"));
    receiveCoinsAction->setToolTip(receiveCoinsAction->statusTip());
    receiveCoinsAction->setCheckable(true);
    receiveCoinsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_3));
    tabGroup->addAction(receiveCoinsAction);

    historyAction = new QAction(platformStyle->SingleColorIcon(":/icons/history"), tr("&Transactions"), this);
    historyAction->setStatusTip(tr("Browse transaction history"));
    historyAction->setToolTip(historyAction->statusTip());
    historyAction->setCheckable(true);
    historyAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_4));
    tabGroup->addAction(historyAction);

    assetsAction = new QAction(platformStyle->SingleColorIcon(":/icons/assets"), tr("&Assets"), this);
    assetsAction->setStatusTip(tr("Issue, reissue and manage Sequentia assets"));
    assetsAction->setToolTip(assetsAction->statusTip());
    assetsAction->setCheckable(true);
    assetsAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_5));
    tabGroup->addAction(assetsAction);

    stakingAction = new QAction(platformStyle->SingleColorIcon(":/icons/staking"), tr("&Staking"), this);
    stakingAction->setStatusTip(tr("Stake Sequence (SEQ) and manage block production"));
    stakingAction->setToolTip(stakingAction->statusTip());
    stakingAction->setCheckable(true);
    stakingAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_6));
    tabGroup->addAction(stakingAction);

    // Sequentia operator tool (menu action, not a tab): view/edit which assets this node
    // accepts for fees.
    feePolicyAction = new QAction(tr("&Fee acceptance…"), this);
    feePolicyAction->setStatusTip(tr("View and edit which assets this node accepts for fee payment"));

    // Sequentia operator tool: launch the bundled price-server sidecar (which keeps the dynamic fee
    // whitelist updated) and open its configuration page in a browser. Node-level; no wallet needed.
    priceServerAction = new QAction(tr("&Price server…"), this);
    priceServerAction->setStatusTip(tr("Start the price-server sidecar and open its configuration page"));
    connect(priceServerAction, &QAction::triggered, this, &BitcoinGUI::launchPriceServer);

#ifdef ENABLE_WALLET
    // These showNormalIfMinimized are needed because Send Coins and Receive Coins
    // can be triggered from the tray menu, and need to show the GUI to be useful.
    connect(overviewAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(overviewAction, &QAction::triggered, this, &BitcoinGUI::gotoOverviewPage);
    connect(sendCoinsAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(sendCoinsAction, &QAction::triggered, [this]{ gotoSendCoinsPage(); });
    connect(receiveCoinsAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(receiveCoinsAction, &QAction::triggered, this, &BitcoinGUI::gotoReceiveCoinsPage);
    connect(historyAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(historyAction, &QAction::triggered, this, &BitcoinGUI::gotoHistoryPage);
    connect(assetsAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(assetsAction, &QAction::triggered, this, &BitcoinGUI::gotoAssetsPage);
    connect(stakingAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(stakingAction, &QAction::triggered, this, &BitcoinGUI::gotoStakingPage);
    connect(feePolicyAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
    connect(feePolicyAction, &QAction::triggered, this, &BitcoinGUI::gotoFeePolicyDialog);
#endif // ENABLE_WALLET

    quitAction = new QAction(tr("E&xit"), this);
    quitAction->setStatusTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);
    aboutAction = new QAction(tr("&About %1").arg(PACKAGE_NAME), this);
    aboutAction->setStatusTip(tr("Show information about %1").arg(PACKAGE_NAME));
    aboutAction->setMenuRole(QAction::AboutRole);
    aboutAction->setEnabled(false);
    aboutQtAction = new QAction(tr("About &Qt"), this);
    aboutQtAction->setStatusTip(tr("Show information about Qt"));
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
    optionsAction = new QAction(tr("&Options…"), this);
    optionsAction->setStatusTip(tr("Modify configuration options for %1").arg(PACKAGE_NAME));
    optionsAction->setMenuRole(QAction::PreferencesRole);
    optionsAction->setEnabled(false);

    encryptWalletAction = new QAction(tr("&Encrypt Wallet…"), this);
    encryptWalletAction->setStatusTip(tr("Encrypt the private keys that belong to your wallet"));
    encryptWalletAction->setCheckable(true);
    backupWalletAction = new QAction(tr("&Backup Wallet…"), this);
    backupWalletAction->setStatusTip(tr("Backup wallet to another location"));
    changePassphraseAction = new QAction(tr("&Change Passphrase…"), this);
    changePassphraseAction->setStatusTip(tr("Change the passphrase used for wallet encryption"));
    signMessageAction = new QAction(tr("Sign &message…"), this);
    signMessageAction->setStatusTip(tr("Sign messages with your %1 addresses to prove you own them").arg("Sequentia"));
    verifyMessageAction = new QAction(tr("&Verify message…"), this);
    verifyMessageAction->setStatusTip(tr("Verify messages to ensure they were signed with specified %1 addresses").arg("Sequentia"));
    m_load_psbt_action = new QAction(tr("&Load PSET from file…"), this);
    m_load_psbt_action->setStatusTip(tr("Load Partially Signed Elements Transaction"));
    m_load_psbt_clipboard_action = new QAction(tr("Load PSET from &clipboard…"), this);
    m_load_psbt_clipboard_action->setStatusTip(tr("Load Partially Signed Elements Transaction from clipboard"));

    openRPCConsoleAction = new QAction(tr("Node window"), this);
    openRPCConsoleAction->setStatusTip(tr("Open node debugging and diagnostic console"));
    // initially disable the debug window menu item
    openRPCConsoleAction->setEnabled(false);
    openRPCConsoleAction->setObjectName("openRPCConsoleAction");

    usedSendingAddressesAction = new QAction(tr("&Sending addresses"), this);
    usedSendingAddressesAction->setStatusTip(tr("Show the list of used sending addresses and labels"));
    usedReceivingAddressesAction = new QAction(tr("&Receiving addresses"), this);
    usedReceivingAddressesAction->setStatusTip(tr("Show the list of used receiving addresses and labels"));

    m_open_wallet_action = new QAction(tr("Open Wallet"), this);
    m_open_wallet_action->setEnabled(false);
    m_open_wallet_action->setStatusTip(tr("Open a wallet"));
    m_open_wallet_menu = new QMenu(this);

    m_close_wallet_action = new QAction(tr("Close Wallet…"), this);
    m_close_wallet_action->setStatusTip(tr("Close wallet"));

    m_create_wallet_action = new QAction(tr("Create Wallet…"), this);
    m_create_wallet_action->setEnabled(false);
    m_create_wallet_action->setStatusTip(tr("Create a new wallet"));

    m_close_all_wallets_action = new QAction(tr("Close All Wallets…"), this);
    m_close_all_wallets_action->setStatusTip(tr("Close all wallets"));

    showHelpMessageAction = new QAction(tr("&Command-line options"), this);
    showHelpMessageAction->setMenuRole(QAction::NoRole);
    showHelpMessageAction->setStatusTip(tr("Show the %1 help message to get a list with possible command-line options").arg(PACKAGE_NAME));

    m_mask_values_action = new QAction(tr("&Mask values"), this);
    m_mask_values_action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_M));
    m_mask_values_action->setStatusTip(tr("Mask the values in the Overview tab"));
    m_mask_values_action->setCheckable(true);

    connect(quitAction, &QAction::triggered, this, &BitcoinGUI::quitRequested);
    connect(aboutAction, &QAction::triggered, this, &BitcoinGUI::aboutClicked);
    connect(aboutQtAction, &QAction::triggered, qApp, QApplication::aboutQt);
    connect(optionsAction, &QAction::triggered, this, &BitcoinGUI::optionsClicked);
    connect(showHelpMessageAction, &QAction::triggered, this, &BitcoinGUI::showHelpMessageClicked);
    connect(openRPCConsoleAction, &QAction::triggered, this, &BitcoinGUI::showDebugWindow);
    // prevents an open debug window from becoming stuck/unusable on client shutdown
    connect(quitAction, &QAction::triggered, rpcConsole, &QWidget::hide);

#ifdef ENABLE_WALLET
    if(walletFrame)
    {
        connect(encryptWalletAction, &QAction::triggered, walletFrame, &WalletFrame::encryptWallet);
        connect(backupWalletAction, &QAction::triggered, walletFrame, &WalletFrame::backupWallet);
        connect(changePassphraseAction, &QAction::triggered, walletFrame, &WalletFrame::changePassphrase);
        connect(signMessageAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
        connect(signMessageAction, &QAction::triggered, [this]{ gotoSignMessageTab(); });
        connect(m_load_psbt_action, &QAction::triggered, [this]{ gotoLoadPSBT(); });
        connect(m_load_psbt_clipboard_action, &QAction::triggered, [this]{ gotoLoadPSBT(true); });
        connect(verifyMessageAction, &QAction::triggered, [this]{ showNormalIfMinimized(); });
        connect(verifyMessageAction, &QAction::triggered, [this]{ gotoVerifyMessageTab(); });
        connect(usedSendingAddressesAction, &QAction::triggered, walletFrame, &WalletFrame::usedSendingAddresses);
        connect(usedReceivingAddressesAction, &QAction::triggered, walletFrame, &WalletFrame::usedReceivingAddresses);
        connect(m_open_wallet_menu, &QMenu::aboutToShow, [this] {
            m_open_wallet_menu->clear();
            for (const std::pair<const std::string, bool>& i : m_wallet_controller->listWalletDir()) {
                const std::string& path = i.first;
                QString name = path.empty() ? QString("["+tr("default wallet")+"]") : QString::fromStdString(path);
                // Menu items remove single &. Single & are shown when && is in
                // the string, but only the first occurrence. So replace only
                // the first & with &&.
                name.replace(name.indexOf(QChar('&')), 1, QString("&&"));
                if (i.second) {
                    // Already loaded. Rather than grey it out -- which read as
                    // "broken" to someone who just wanted to switch back to it --
                    // let it switch to that wallet, which is what picking an
                    // already-open wallet plainly ought to do. The models live in
                    // the sidebar selector; find the one whose name matches.
                    WalletModel* loaded = nullptr;
                    if (m_wallet_selector) {
                        for (int idx = 0; idx < m_wallet_selector->count(); ++idx) {
                            WalletModel* wm = m_wallet_selector->itemData(idx).value<WalletModel*>();
                            if (wm && wm->getWalletName().toStdString() == path) { loaded = wm; break; }
                        }
                    }
                    QAction* action = m_open_wallet_menu->addAction(name + "  " + tr("(open)"));
                    if (loaded) {
                        connect(action, &QAction::triggered, this, [this, loaded] { setCurrentWallet(loaded); });
                    } else {
                        action->setEnabled(false);
                    }
                    continue;
                }

                QAction* action = m_open_wallet_menu->addAction(name);
                connect(action, &QAction::triggered, [this, path] {
                    auto activity = new OpenWalletActivity(m_wallet_controller, this);
                    connect(activity, &OpenWalletActivity::opened, this, &BitcoinGUI::setCurrentWallet);
                    activity->open(path);
                });
            }
            if (m_open_wallet_menu->isEmpty()) {
                QAction* action = m_open_wallet_menu->addAction(tr("No wallets available"));
                action->setEnabled(false);
            }
        });
        connect(m_close_wallet_action, &QAction::triggered, [this] {
            m_wallet_controller->closeWallet(walletFrame->currentWalletModel(), this);
        });
        connect(m_create_wallet_action, &QAction::triggered, [this] {
            auto activity = new CreateWalletActivity(m_wallet_controller, this);
            connect(activity, &CreateWalletActivity::created, this, &BitcoinGUI::setCurrentWallet);
            activity->create();
        });
        connect(m_close_all_wallets_action, &QAction::triggered, [this] {
            m_wallet_controller->closeAllWallets(this);
        });
        connect(m_mask_values_action, &QAction::toggled, this, &BitcoinGUI::setPrivacy);
    }
#endif // ENABLE_WALLET

    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_C), this), &QShortcut::activated, this, &BitcoinGUI::showDebugWindowActivateConsole);
    connect(new QShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_D), this), &QShortcut::activated, this, &BitcoinGUI::showDebugWindow);
}

void BitcoinGUI::createMenuBar()
{
#ifdef Q_OS_MAC
    // Create a decoupled menu bar on Mac which stays even if the window is closed
    appMenuBar = new QMenuBar();
#else
    // Get the main window's menu bar on other platforms
    appMenuBar = menuBar();
#endif

    // Configure the menus
    QMenu *file = appMenuBar->addMenu(tr("&File"));
    if(walletFrame)
    {
        file->addAction(m_create_wallet_action);
        file->addAction(m_open_wallet_action);
        file->addAction(m_close_wallet_action);
        file->addAction(m_close_all_wallets_action);
        file->addSeparator();
        file->addAction(backupWalletAction);
        file->addAction(signMessageAction);
        file->addAction(verifyMessageAction);
        file->addAction(m_load_psbt_action);
        file->addAction(m_load_psbt_clipboard_action);
        file->addSeparator();
    }
    file->addAction(quitAction);

    QMenu *settings = appMenuBar->addMenu(tr("&Settings"));
    if(walletFrame)
    {
        settings->addAction(encryptWalletAction);
        settings->addAction(changePassphraseAction);
        settings->addSeparator();
        settings->addAction(m_mask_values_action);
        settings->addAction(feePolicyAction);
        settings->addSeparator();
    }
    settings->addAction(optionsAction);
    settings->addSeparator();
    settings->addAction(priceServerAction);

    QMenu* window_menu = appMenuBar->addMenu(tr("&Window"));

    QAction* minimize_action = window_menu->addAction(tr("&Minimize"));
    minimize_action->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_M));
    connect(minimize_action, &QAction::triggered, [] {
        QApplication::activeWindow()->showMinimized();
    });
    connect(qApp, &QApplication::focusWindowChanged, this, [minimize_action] (QWindow* window) {
        minimize_action->setEnabled(window != nullptr && (window->flags() & Qt::Dialog) != Qt::Dialog && window->windowState() != Qt::WindowMinimized);
    });

#ifdef Q_OS_MAC
    QAction* zoom_action = window_menu->addAction(tr("Zoom"));
    connect(zoom_action, &QAction::triggered, [] {
        QWindow* window = qApp->focusWindow();
        if (window->windowState() != Qt::WindowMaximized) {
            window->showMaximized();
        } else {
            window->showNormal();
        }
    });

    connect(qApp, &QApplication::focusWindowChanged, this, [zoom_action] (QWindow* window) {
        zoom_action->setEnabled(window != nullptr);
    });
#endif

    if (walletFrame) {
#ifdef Q_OS_MAC
        window_menu->addSeparator();
        QAction* main_window_action = window_menu->addAction(tr("Main Window"));
        connect(main_window_action, &QAction::triggered, [this] {
            GUIUtil::bringToFront(this);
        });
#endif
        window_menu->addSeparator();
        window_menu->addAction(usedSendingAddressesAction);
        window_menu->addAction(usedReceivingAddressesAction);
    }

    window_menu->addSeparator();
    for (RPCConsole::TabTypes tab_type : rpcConsole->tabs()) {
        QAction* tab_action = window_menu->addAction(rpcConsole->tabTitle(tab_type));
        tab_action->setShortcut(rpcConsole->tabShortcut(tab_type));
        connect(tab_action, &QAction::triggered, [this, tab_type] {
            rpcConsole->setTabFocus(tab_type);
            showDebugWindow();
        });
    }

    QMenu *help = appMenuBar->addMenu(tr("&Help"));
    help->addAction(showHelpMessageAction);
    help->addSeparator();
    help->addAction(aboutAction);
    help->addAction(aboutQtAction);
}

void BitcoinGUI::createToolBars()
{
    if(walletFrame)
    {
        // A vertical sidebar rather than a strip across the top: the tab names read
        // as a list, the window keeps its height for content, and there is room for
        // the wallet selector to sit under the tabs instead of squeezing them.
        QToolBar *toolbar = new QToolBar(tr("Tabs toolbar"), this);
        addToolBar(Qt::LeftToolBarArea, toolbar);
        appToolBar = toolbar;
        toolbar->setObjectName("appToolBar"); // styled as the sidebar in sequentia.css
        toolbar->setMovable(false);
        toolbar->setOrientation(Qt::Vertical);
        toolbar->setIconSize(QSize(18, 18));
        overviewAction->setChecked(true);
        // The tabs are QPushButtons, not the QToolButtons addAction() would make.
        // A QToolButton centres its icon+text as a group, so short labels float in
        // the middle of a full-width button and the column reads ragged; Qt gives
        // no way to left-align that under an active stylesheet (a QProxyStyle is
        // bypassed by QStyleSheetStyle). A QPushButton honours `text-align: left`
        // in the stylesheet, so the icons and labels line up. Each button mirrors
        // its action: clicking it triggers the action (page switch), and the
        // action toggling back keeps the button's checked state in sync.
        QButtonGroup* navGroup = new QButtonGroup(this);
        navGroup->setExclusive(true);
        for (QAction* action : {overviewAction, sendCoinsAction, receiveCoinsAction,
                                historyAction, assetsAction, stakingAction}) {
            // Recolour the icon to the muted text colour: the action's icon is
            // single-colour amber, and a QPushButton shows it at full strength for
            // every state, which made every tab look selected. Amber is left to
            // mark the current tab via its rail and label.
            QPushButton* button = new QPushButton(platformStyle->TextColorIcon(action->icon()), action->text(), toolbar);
            button->setCheckable(true);
            button->setChecked(action->isChecked());
            button->setIconSize(QSize(18, 18));
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(action->toolTip());
            navGroup->addButton(button);
            toolbar->addWidget(button);
            connect(button, &QPushButton::clicked, action, &QAction::trigger);
            connect(action, &QAction::toggled, button, &QPushButton::setChecked);
        }

#ifdef ENABLE_WALLET
        QWidget *spacer = new QWidget();
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        toolbar->addWidget(spacer);

        m_wallet_selector = new QComboBox();
        m_wallet_selector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        connect(m_wallet_selector, qOverload<int>(&QComboBox::currentIndexChanged), this, &BitcoinGUI::setCurrentWalletBySelectorIndex);

        m_wallet_selector_label = new QLabel();
        m_wallet_selector_label->setText(tr("Wallet:") + " ");
        m_wallet_selector_label->setBuddy(m_wallet_selector);

        m_wallet_selector_label_action = appToolBar->addWidget(m_wallet_selector_label);
        m_wallet_selector_action = appToolBar->addWidget(m_wallet_selector);

        m_wallet_selector_label_action->setVisible(false);
        m_wallet_selector_action->setVisible(false);
#endif
    }
}

void BitcoinGUI::setClientModel(ClientModel *_clientModel, interfaces::BlockAndHeaderTipInfo* tip_info)
{
    this->clientModel = _clientModel;
    if(_clientModel)
    {
        // Create system tray menu (or setup the dock menu) that late to prevent users from calling actions,
        // while the client has not yet fully loaded
        createTrayIconMenu();

        // Keep up to date with client
        setNetworkActive(m_node.getNetworkActive());
        connect(connectionsControl, &GUIUtil::ClickableLabel::clicked, [this] {
            GUIUtil::PopupMenu(m_network_context_menu, QCursor::pos());
        });
        connect(_clientModel, &ClientModel::numConnectionsChanged, this, &BitcoinGUI::setNumConnections);
        connect(_clientModel, &ClientModel::networkActiveChanged, this, &BitcoinGUI::setNetworkActive);

        modalOverlay->setKnownBestHeight(tip_info->header_height, QDateTime::fromSecsSinceEpoch(tip_info->header_time));
        setNumBlocks(tip_info->block_height, QDateTime::fromSecsSinceEpoch(tip_info->block_time), tip_info->verification_progress, false, SynchronizationState::INIT_DOWNLOAD);
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &BitcoinGUI::setNumBlocks);

        // SEQUENTIA: watch for a Bitcoin-driven stall of the chain tip and
        // say so in the status bar (see updateAnchorWaitStatus). The initial
        // setNumBlocks call above seeded m_last_tip_advance from the tip
        // block's timestamp, so a chain already stalled before the GUI opened
        // is noticed on the first tick.
        if (!m_anchor_wait_timer) {
            m_anchor_wait_timer = new QTimer(this);
            m_anchor_wait_timer->setInterval(15 * 1000);
            connect(m_anchor_wait_timer, &QTimer::timeout, this, &BitcoinGUI::updateAnchorWaitStatus);
        }
        m_anchor_wait_timer->start();

        // Receive and report messages from client model
        connect(_clientModel, &ClientModel::message, [this](const QString &title, const QString &message, unsigned int style){
            this->message(title, message, style);
        });

        // Show progress dialog
        connect(_clientModel, &ClientModel::showProgress, this, &BitcoinGUI::showProgress);

        rpcConsole->setClientModel(_clientModel, tip_info->block_height, tip_info->block_time, tip_info->verification_progress);

        updateProxyIcon();

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->setClientModel(_clientModel);
        }
#endif // ENABLE_WALLET
        refCurrencyControl->setOptionsModel(_clientModel->getOptionsModel());

        OptionsModel* optionsModel = _clientModel->getOptionsModel();
        if (optionsModel && trayIcon) {
            // be aware of the tray icon disable state change reported by the OptionsModel object.
            connect(optionsModel, &OptionsModel::showTrayIconChanged, trayIcon, &QSystemTrayIcon::setVisible);

            // initialize the disable state of the tray icon with the current value in the model.
            trayIcon->setVisible(optionsModel->getShowTrayIcon());
        }
    } else {
        if(trayIconMenu)
        {
            // Disable context menu on tray icon
            trayIconMenu->clear();
        }
        if (m_anchor_wait_timer) m_anchor_wait_timer->stop();
        // Propagate cleared model to child objects
        rpcConsole->setClientModel(nullptr);
#ifdef ENABLE_WALLET
        if (walletFrame)
        {
            walletFrame->setClientModel(nullptr);
        }
#endif // ENABLE_WALLET
        refCurrencyControl->setOptionsModel(nullptr);
    }
}

#ifdef ENABLE_WALLET
void BitcoinGUI::setWalletController(WalletController* wallet_controller)
{
    assert(!m_wallet_controller);
    assert(wallet_controller);

    m_wallet_controller = wallet_controller;

    m_create_wallet_action->setEnabled(true);
    m_open_wallet_action->setEnabled(true);
    m_open_wallet_action->setMenu(m_open_wallet_menu);

    GUIUtil::ExceptionSafeConnect(wallet_controller, &WalletController::walletAdded, this, &BitcoinGUI::addWallet);
    connect(wallet_controller, &WalletController::walletRemoved, this, &BitcoinGUI::removeWallet);

    auto activity = new LoadWalletsActivity(m_wallet_controller, this);
    activity->load();
}

WalletController* BitcoinGUI::getWalletController()
{
    return m_wallet_controller;
}

void BitcoinGUI::addWallet(WalletModel* walletModel)
{
    if (!walletFrame) return;

    WalletView* wallet_view = new WalletView(walletModel, platformStyle, walletFrame);
    if (!walletFrame->addView(wallet_view)) return;

    rpcConsole->addWallet(walletModel);
    if (m_wallet_selector->count() == 0) {
        setWalletActionsEnabled(true);
    } else if (m_wallet_selector->count() == 1) {
        m_wallet_selector_label_action->setVisible(true);
        m_wallet_selector_action->setVisible(true);
    }

    connect(wallet_view, &WalletView::outOfSyncWarningClicked, this, &BitcoinGUI::showModalOverlay);
    connect(wallet_view, &WalletView::transactionClicked, this, &BitcoinGUI::gotoHistoryPage);
    connect(wallet_view, &WalletView::coinsSent, this, &BitcoinGUI::gotoHistoryPage);
    connect(wallet_view, &WalletView::message, [this](const QString& title, const QString& message, unsigned int style) {
        this->message(title, message, style);
    });
    connect(wallet_view, &WalletView::encryptionStatusChanged, this, &BitcoinGUI::updateWalletStatus);
    connect(wallet_view, &WalletView::incomingTransaction, this, &BitcoinGUI::incomingTransaction);
    connect(this, &BitcoinGUI::setPrivacy, wallet_view, &WalletView::setPrivacy);
    wallet_view->setPrivacy(isPrivacyModeActivated());
    const QString display_name = walletModel->getDisplayName();
    m_wallet_selector->addItem(display_name, QVariant::fromValue(walletModel));
}

void BitcoinGUI::removeWallet(WalletModel* walletModel)
{
    if (!walletFrame) return;

    labelWalletHDStatusIcon->hide();
    labelWalletEncryptionIcon->hide();

    int index = m_wallet_selector->findData(QVariant::fromValue(walletModel));
    m_wallet_selector->removeItem(index);
    if (m_wallet_selector->count() == 0) {
        setWalletActionsEnabled(false);
        overviewAction->setChecked(true);
    } else if (m_wallet_selector->count() == 1) {
        m_wallet_selector_label_action->setVisible(false);
        m_wallet_selector_action->setVisible(false);
    }
    rpcConsole->removeWallet(walletModel);
    walletFrame->removeWallet(walletModel);
    updateWindowTitle();
}

void BitcoinGUI::setCurrentWallet(WalletModel* wallet_model)
{
    if (!walletFrame) return;
    walletFrame->setCurrentWallet(wallet_model);
    for (int index = 0; index < m_wallet_selector->count(); ++index) {
        if (m_wallet_selector->itemData(index).value<WalletModel*>() == wallet_model) {
            m_wallet_selector->setCurrentIndex(index);
            break;
        }
    }
    updateWindowTitle();
}

void BitcoinGUI::setCurrentWalletBySelectorIndex(int index)
{
    WalletModel* wallet_model = m_wallet_selector->itemData(index).value<WalletModel*>();
    if (wallet_model) setCurrentWallet(wallet_model);
}

void BitcoinGUI::removeAllWallets()
{
    if(!walletFrame)
        return;
    setWalletActionsEnabled(false);
    walletFrame->removeAllWallets();
}
#endif // ENABLE_WALLET

void BitcoinGUI::setWalletActionsEnabled(bool enabled)
{
    overviewAction->setEnabled(enabled);
    sendCoinsAction->setEnabled(enabled);
    receiveCoinsAction->setEnabled(enabled);
    historyAction->setEnabled(enabled);
    assetsAction->setEnabled(enabled);
    stakingAction->setEnabled(enabled);
    feePolicyAction->setEnabled(enabled);
    encryptWalletAction->setEnabled(enabled);
    backupWalletAction->setEnabled(enabled);
    changePassphraseAction->setEnabled(enabled);
    signMessageAction->setEnabled(enabled);
    verifyMessageAction->setEnabled(enabled);
    usedSendingAddressesAction->setEnabled(enabled);
    usedReceivingAddressesAction->setEnabled(enabled);
    m_close_wallet_action->setEnabled(enabled);
    m_close_all_wallets_action->setEnabled(enabled);
}

void BitcoinGUI::createTrayIcon()
{
    assert(QSystemTrayIcon::isSystemTrayAvailable());

#ifndef Q_OS_MAC
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon = new QSystemTrayIcon(m_network_style->getTrayAndWindowIcon(), this);
        QString toolTip = tr("%1 client").arg(PACKAGE_NAME) + " " + m_network_style->getTitleAddText();
        trayIcon->setToolTip(toolTip);
    }
#endif
}

void BitcoinGUI::createTrayIconMenu()
{
#ifndef Q_OS_MAC
    if (!trayIcon) return;
#endif // Q_OS_MAC

    // Configuration of the tray icon (or Dock icon) menu.
    QAction* show_hide_action{nullptr};
#ifndef Q_OS_MAC
    // Note: On macOS, the Dock icon's menu already has Show / Hide action.
    show_hide_action = trayIconMenu->addAction(QString(), this, &BitcoinGUI::toggleHidden);
    trayIconMenu->addSeparator();
#endif // Q_OS_MAC

    QAction* send_action{nullptr};
    QAction* receive_action{nullptr};
    QAction* sign_action{nullptr};
    QAction* verify_action{nullptr};
    if (enableWallet) {
        send_action = trayIconMenu->addAction(sendCoinsAction->text(), sendCoinsAction, &QAction::trigger);
        receive_action = trayIconMenu->addAction(receiveCoinsAction->text(), receiveCoinsAction, &QAction::trigger);
        trayIconMenu->addSeparator();
        sign_action = trayIconMenu->addAction(signMessageAction->text(), signMessageAction, &QAction::trigger);
        verify_action = trayIconMenu->addAction(verifyMessageAction->text(), verifyMessageAction, &QAction::trigger);
        trayIconMenu->addSeparator();
    }
    QAction* options_action = trayIconMenu->addAction(optionsAction->text(), optionsAction, &QAction::trigger);
    options_action->setMenuRole(QAction::PreferencesRole);
    QAction* node_window_action = trayIconMenu->addAction(openRPCConsoleAction->text(), openRPCConsoleAction, &QAction::trigger);
    QAction* quit_action{nullptr};
#ifndef Q_OS_MAC
    // Note: On macOS, the Dock icon's menu already has Quit action.
    trayIconMenu->addSeparator();
    quit_action = trayIconMenu->addAction(quitAction->text(), quitAction, &QAction::trigger);

    trayIcon->setContextMenu(trayIconMenu.get());
    connect(trayIcon, &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            // Click on system tray icon triggers show/hide of the main window
            toggleHidden();
        }
    });
#else
    // Note: On macOS, the Dock icon is used to provide the tray's functionality.
    MacDockIconHandler* dockIconHandler = MacDockIconHandler::instance();
    connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, [this] {
        show();
        activateWindow();
    });
    trayIconMenu->setAsDockMenu();
#endif // Q_OS_MAC

    connect(
        // Using QSystemTrayIcon::Context is not reliable.
        // See https://bugreports.qt.io/browse/QTBUG-91697
        trayIconMenu.get(), &QMenu::aboutToShow,
        [this, show_hide_action, send_action, receive_action, sign_action, verify_action, options_action, node_window_action, quit_action] {
            if (show_hide_action) show_hide_action->setText(
                (!isHidden() && !isMinimized() && !GUIUtil::isObscured(this)) ?
                    tr("&Hide") :
                    tr("S&how"));
            if (QApplication::activeModalWidget()) {
                for (QAction* a : trayIconMenu.get()->actions()) {
                    a->setEnabled(false);
                }
            } else {
                if (show_hide_action) show_hide_action->setEnabled(true);
                if (enableWallet) {
                    send_action->setEnabled(sendCoinsAction->isEnabled());
                    receive_action->setEnabled(receiveCoinsAction->isEnabled());
                    sign_action->setEnabled(signMessageAction->isEnabled());
                    verify_action->setEnabled(verifyMessageAction->isEnabled());
                }
                options_action->setEnabled(optionsAction->isEnabled());
                node_window_action->setEnabled(openRPCConsoleAction->isEnabled());
                if (quit_action) quit_action->setEnabled(true);
            }
        });
}

void BitcoinGUI::optionsClicked()
{
    openOptionsDialogWithTab(OptionsDialog::TAB_MAIN);
}

void BitcoinGUI::aboutClicked()
{
    if(!clientModel)
        return;

    auto dlg = new HelpMessageDialog(this, /* about */ true);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void BitcoinGUI::showDebugWindow()
{
    GUIUtil::bringToFront(rpcConsole);
    Q_EMIT consoleShown(rpcConsole);
}

void BitcoinGUI::showDebugWindowActivateConsole()
{
    rpcConsole->setTabFocus(RPCConsole::TabTypes::CONSOLE);
    showDebugWindow();
}

void BitcoinGUI::showHelpMessageClicked()
{
    GUIUtil::bringToFront(helpMessageDialog);
}

#ifdef ENABLE_WALLET
void BitcoinGUI::gotoOverviewPage()
{
    overviewAction->setChecked(true);
    if (walletFrame) walletFrame->gotoOverviewPage();
}

void BitcoinGUI::gotoHistoryPage()
{
    historyAction->setChecked(true);
    if (walletFrame) walletFrame->gotoHistoryPage();
}

void BitcoinGUI::gotoReceiveCoinsPage()
{
    receiveCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoReceiveCoinsPage();
}

void BitcoinGUI::gotoAssetsPage()
{
    assetsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoAssetsPage();
}

void BitcoinGUI::gotoStakingPage()
{
    stakingAction->setChecked(true);
    if (walletFrame) walletFrame->gotoStakingPage();
}

void BitcoinGUI::gotoFeePolicyDialog()
{
    if (walletFrame) walletFrame->gotoFeePolicyDialog();
}

void BitcoinGUI::launchPriceServer()
{
    showNormalIfMinimized();
    const QString appDir = QCoreApplication::applicationDirPath();

    // Locate the bundled sidecar by walking UP from the binary: it lives in price-server/ next to a
    // packaged binary, or in contrib/price-server/ inside a (possibly out-of-tree) build tree.
    auto findUp = [&](const QStringList& rels) -> QString {
        QDir d(appDir);
        for (int i = 0; i < 7; ++i) {
            for (const QString& rel : rels) {
                const QString c = d.absoluteFilePath(rel);
                if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
            }
            if (!d.cdUp()) break;
        }
        return QString();
    };

    const QString script = findUp({QStringLiteral("price-server/price_server.py"),
                                   QStringLiteral("contrib/price-server/price_server.py")});
    if (script.isEmpty()) {
        QMessageBox::warning(this, tr("Price server"),
            tr("Could not find the price-server script (price_server.py). It ships bundled with the node."));
        return;
    }
    const QString sdir = QFileInfo(script).absolutePath();

    // A Python interpreter: bundled next to the script (packaged build), else the system python3.
    QString python;
    const QStringList pyCands{ sdir + "/python/python3", sdir + "/python/python.exe",
                              appDir + "/python/python3", appDir + "/python/python.exe" };
    for (const QString& c : pyCands) {
        if (QFileInfo::exists(c)) { python = QFileInfo(c).absoluteFilePath(); break; }
    }
    if (python.isEmpty()) python = QStringLiteral("python3");

    // Per-user config in the app data dir; seed it from the bundled config.example.json on first run.
    // (Do NOT use gen-price-config.py — it writes a demo config to /root and prints only a summary.)
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty()) QDir().mkpath(dataDir);
    const QString cfg = (dataDir.isEmpty() ? sdir : dataDir) + "/price-server.json";
    if (!QFileInfo::exists(cfg)) {
        const QString example = findUp({QStringLiteral("price-server/config.example.json"),
                                        QStringLiteral("contrib/price-server/config.example.json")});
        if (example.isEmpty() || !QFile::copy(example, cfg)) {
            QMessageBox::warning(this, tr("Price server"),
                tr("Could not create a default price-server config at %1.").arg(cfg));
            return;
        }
    }

    const int uiPort = 8089;
    const QString url = QString("http://127.0.0.1:%1/").arg(uiPort);

    // Already running (from an earlier click)? Just reopen the page — never spawn a
    // second copy.
    if (m_price_server && m_price_server->state() != QProcess::NotRunning) {
        QDesktopServices::openUrl(QUrl(url));
        return;
    }

    // Launch as a TRACKED child bound to LOOPBACK ONLY (never 0.0.0.0), so it cannot
    // outlive the GUI: stopPriceServer() (called from the destructor) terminates it,
    // which triggers the sidecar's own clean shutdown (clearing the dynamic fee
    // whitelist). This replaces the old startDetached(), which orphaned the process.
    if (m_price_server) { m_price_server->deleteLater(); m_price_server = nullptr; }
    m_price_server = new QProcess(this);
    m_price_server->setWorkingDirectory(sdir);
    m_price_server->start(python, {script, "--config", cfg,
                                   "--ui-port", QString::number(uiPort), "--ui-host", "127.0.0.1"});
    if (!m_price_server->waitForStarted(4000)) {
        QMessageBox::warning(this, tr("Price server"),
            tr("Failed to start the price server using '%1'. Ensure Python is available.").arg(python));
        m_price_server->deleteLater(); m_price_server = nullptr;
        return;
    }

#ifdef Q_OS_WIN
    // Put the sidecar in a kill-on-close job object. stopPriceServer() only runs on a
    // clean exit; if the GUI dies any other way (crash, forced termination) the OS
    // closes the job handle and kills the sidecar with it, so it can never linger as
    // an orphan. The handle is deliberately kept open for the life of the process.
    static HANDLE job_handle = []() -> HANDLE {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
                CloseHandle(h);
                h = nullptr;
            }
        }
        return h;
    }();
    if (job_handle) {
        if (HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                      static_cast<DWORD>(m_price_server->processId()))) {
            AssignProcessToJobObject(job_handle, proc);
            CloseHandle(proc);
        }
    }
#endif

    // Poll the UI port until the sidecar actually binds, then open its configuration
    // page. A bound port (not just "python launched") confirms the script started
    // cleanly — a bad config or missing dependency exits immediately. Open on the
    // first successful connection; warn if it never comes up.
    if (statusBar()) statusBar()->showMessage(tr("Price server starting…"), 3000);
    QTimer* pollTimer = new QTimer(this);
    auto attempts = std::make_shared<int>(0);
    connect(pollTimer, &QTimer::timeout, this, [this, pollTimer, attempts, uiPort, url]() {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(uiPort));
        const bool up = probe.waitForConnected(200);
        probe.abort();
        if (up) {
            pollTimer->stop();
            pollTimer->deleteLater();
            QDesktopServices::openUrl(QUrl(url));
        } else if (++(*attempts) >= 30) { // ~9s of 300ms polls
            pollTimer->stop();
            pollTimer->deleteLater();
            QMessageBox::warning(this, tr("Price server"),
                tr("The price server did not come up at %1. It likely failed to start; check that "
                   "Python and its dependencies are installed and that the price-server config is valid.").arg(url));
        }
    });
    pollTimer->start(300);
}

void BitcoinGUI::stopPriceServer()
{
    if (!m_price_server) return;
    if (m_price_server->state() != QProcess::NotRunning) {
        // SIGTERM lets the sidecar run its own clean shutdown (it clears the dynamic
        // fee whitelist on the node); fall back to kill() if it doesn't exit promptly.
        m_price_server->terminate();
        if (!m_price_server->waitForFinished(3000)) {
            m_price_server->kill();
            m_price_server->waitForFinished(1000);
        }
    }
    delete m_price_server;
    m_price_server = nullptr;
}

void BitcoinGUI::gotoSendCoinsPage(QString addr)
{
    sendCoinsAction->setChecked(true);
    if (walletFrame) walletFrame->gotoSendCoinsPage(addr);
}

void BitcoinGUI::gotoSignMessageTab(QString addr)
{
    if (walletFrame) walletFrame->gotoSignMessageTab(addr);
}

void BitcoinGUI::gotoVerifyMessageTab(QString addr)
{
    if (walletFrame) walletFrame->gotoVerifyMessageTab(addr);
}
void BitcoinGUI::gotoLoadPSBT(bool from_clipboard)
{
    if (walletFrame) walletFrame->gotoLoadPSBT(from_clipboard);
}
#endif // ENABLE_WALLET

void BitcoinGUI::updateNetworkState()
{
    int count = clientModel->getNumConnections();
    QString icon;
    switch(count)
    {
    case 0: icon = ":/icons/connect_0"; break;
    case 1: case 2: case 3: icon = ":/icons/connect_1"; break;
    case 4: case 5: case 6: icon = ":/icons/connect_2"; break;
    case 7: case 8: case 9: icon = ":/icons/connect_3"; break;
    default: icon = ":/icons/connect_4"; break;
    }

    QString tooltip;

    if (m_node.getNetworkActive()) {
        //: A substring of the tooltip.
        tooltip = tr("%n active connection(s) to %1 network", "", count).arg("Sequentia");
    } else {
        //: A substring of the tooltip.
        tooltip = tr("Network activity disabled.");
        icon = ":/icons/network_disabled";
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QLatin1String("<nobr>") + tooltip + QLatin1String("<br>") +
              //: A substring of the tooltip. "More actions" are available via the context menu.
              tr("Click for more actions.") + QLatin1String("</nobr>");
    connectionsControl->setToolTip(tooltip);

    connectionsControl->setThemedPixmap(icon, STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
}

void BitcoinGUI::setNumConnections(int count)
{
    updateNetworkState();
}

void BitcoinGUI::setNetworkActive(bool network_active)
{
    updateNetworkState();
    m_network_context_menu->clear();
    m_network_context_menu->addAction(
        //: A context menu item. The "Peers tab" is an element of the "Node window".
        tr("Show Peers tab"),
        [this] {
            rpcConsole->setTabFocus(RPCConsole::TabTypes::PEERS);
            showDebugWindow();
        });
    m_network_context_menu->addAction(
        network_active ?
            //: A context menu item.
            tr("Disable network activity") :
            //: A context menu item. The network activity was disabled previously.
            tr("Enable network activity"),
        [this, new_state = !network_active] { m_node.setNetworkActive(new_state); });
}

void BitcoinGUI::updateHeadersSyncProgressLabel()
{
    int64_t headersTipTime = clientModel->getHeaderTipTime();
    int headersTipHeight = clientModel->getHeaderTipHeight();
    int estHeadersLeft = (GetTime() - headersTipTime) / Params().GetConsensus().nPowTargetSpacing;
    if (estHeadersLeft > HEADER_HEIGHT_DELTA_SYNC)
        progressBarLabel->setText(tr("Syncing Headers (%1%)…").arg(QString::number(100.0 / (headersTipHeight+estHeadersLeft)*headersTipHeight, 'f', 1)));
}

// SEQUENTIA: when the chain pauses because of Bitcoin — the tip's anchor is
// off the Bitcoin best chain (a Bitcoin fork/reorganization being settled),
// rival branches are being rejected at the PoS finality gate, or the Bitcoin
// daemon is unreachable — the status bar would otherwise show nothing at all
// ("up to date" hides the progress texts until MAX_BLOCK_TIME_GAP, 90 min).
// Say explicitly that the node is waiting for Bitcoin, so users know the
// right action is simply to wait (incident 2026-07-11 §8.3).
void BitcoinGUI::updateAnchorWaitStatus()
{
    if (!clientModel) return;

    // No block for this long counts as a stall: 10 slots at the 30 s block
    // interval — a pause this long means production is blocked, not unlucky.
    constexpr int64_t STALL_SECS = 5 * 60;
    // A finality-gate rejection within this window means the fork contest is
    // still live (rivals are re-offered roughly every 30 s while it lasts).
    constexpr int64_t FORK_REJECT_RECENT_SECS = 10 * 60;

    const int64_t now = QDateTime::currentSecsSinceEpoch();
    QString text;
    QString explain;
    // Cheap wall-clock test first: outside a stall the node is not queried at
    // all (the anchor check may round-trip to the Bitcoin daemon).
    if (m_last_tip_advance > 0 && now - m_last_tip_advance >= STALL_SECS) {
        const interfaces::AnchorTipState state = m_node.getAnchorTipState();
        const bool fork_contested = state.last_finality_fork_rejection > 0 &&
                                    now - state.last_finality_fork_rejection <= FORK_REJECT_RECENT_SECS;
        if (state.validated && (!state.anchor_ok || fork_contested)) {
            if (state.no_connection && !fork_contested) {
                text = tr("Waiting for the Bitcoin connection…");
                explain = tr("New blocks are paused because the Bitcoin program this node works with cannot be reached.") + QString("<br>") +
                          tr("Open Bitcoin Core and leave it running; blocks resume on their own once it is reachable again.");
            } else {
                text = tr("Waiting for the Bitcoin network to settle…");
                explain = tr("Sequentia records its history on the Bitcoin network, and Bitcoin is currently settling on a recent change.") + QString("<br>") +
                          tr("New blocks are paused and resume automatically, usually within minutes. Your funds are safe; no action is needed.");
            }
        }
    }

    if (!text.isEmpty()) {
        m_anchor_wait_active = true;
        progressBarLabel->setText(text);
        progressBarLabel->setToolTip(explain);
        labelBlocksIcon->setToolTip(explain);
        progressBarLabel->setVisible(true);
    } else if (m_anchor_wait_active) {
        // Condition cleared without a new block yet: restore the normal
        // status-bar state (setNumBlocks repaints fully on the next block).
        m_anchor_wait_active = false;
        progressBarLabel->setVisible(progressBar->isVisible());
    }
}

void BitcoinGUI::openOptionsDialogWithTab(OptionsDialog::Tab tab)
{
    if (!clientModel || !clientModel->getOptionsModel())
        return;

    auto dlg = new OptionsDialog(this, enableWallet);
    connect(dlg, &OptionsDialog::quitOnReset, this, &BitcoinGUI::quitRequested);
    dlg->setCurrentTab(tab);
    dlg->setModel(clientModel->getOptionsModel());
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void BitcoinGUI::setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool header, SynchronizationState sync_state)
{
// Disabling macOS App Nap on initial sync, disk and reindex operations.
#ifdef Q_OS_MAC
    if (sync_state == SynchronizationState::POST_INIT) {
        m_app_nap_inhibitor->enableAppNap();
    } else {
        m_app_nap_inhibitor->disableAppNap();
    }
#endif

    if (modalOverlay)
    {
        if (header)
            modalOverlay->setKnownBestHeight(count, blockDate);
        else
            modalOverlay->tipUpdate(count, blockDate, nVerificationProgress);
    }
    if (!clientModel)
        return;

    // SEQUENTIA: a (non-header) tip update means the chain is moving again;
    // note when, and retire any "waiting for Bitcoin" stall notice — the
    // normal paths below repaint the status bar. The block's own timestamp
    // (clamped to now) is used instead of the wall clock so that at startup
    // a chain that was already stalled before the GUI opened is noticed on
    // the first timer tick rather than a full stall period later.
    if (!header) {
        m_last_tip_advance = qMin<qint64>(blockDate.toSecsSinceEpoch(), QDateTime::currentSecsSinceEpoch());
        m_anchor_wait_active = false;
    }

    // Prevent orphan statusbar messages (e.g. hover Quit in main menu, wait until chain-sync starts -> garbled text)
    statusBar()->clearMessage();

    // Acquire current block source
    enum BlockSource blockSource = clientModel->getBlockSource();
    switch (blockSource) {
        case BlockSource::NETWORK:
            if (header) {
                updateHeadersSyncProgressLabel();
                return;
            }
            progressBarLabel->setText(tr("Synchronizing with network…"));
            updateHeadersSyncProgressLabel();
            break;
        case BlockSource::DISK:
            if (header) {
                progressBarLabel->setText(tr("Indexing blocks on disk…"));
            } else {
                progressBarLabel->setText(tr("Processing blocks on disk…"));
            }
            break;
        case BlockSource::REINDEX:
            progressBarLabel->setText(tr("Reindexing blocks on disk…"));
            break;
        case BlockSource::NONE:
            if (header) {
                return;
            }
            progressBarLabel->setText(tr("Connecting to peers…"));
            break;
    }

    QString tooltip;

    QDateTime currentDate = QDateTime::currentDateTime();
    qint64 secs = blockDate.secsTo(currentDate);

    tooltip = tr("Processed %n block(s) of transaction history.", "", count);

    // Set icon state: spinning if catching up, tick otherwise
    if (secs < MAX_BLOCK_TIME_GAP) {
        tooltip = tr("Up to date") + QString(".<br>") + tooltip;
        labelBlocksIcon->setThemedPixmap(QStringLiteral(":/icons/synced"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->showOutOfSyncWarning(false);
            modalOverlay->showHide(true, true);
        }
#endif // ENABLE_WALLET

        progressBarLabel->setVisible(false);
        progressBar->setVisible(false);
    }
    else
    {
        QString timeBehindText = GUIUtil::formatNiceTimeOffset(secs);

        // SEQUENTIA: report catch-up as blocks remaining plus a percentage
        // instead of upstream's wall-clock "%1 behind" (block timestamps make
        // "18 hours behind" sound alarming when the actual catch-up is minutes
        // of download and validation).
        const int header_height = clientModel->getHeaderTipHeight();
        const QString percent_done = QString::number(nVerificationProgress * 100.0, 'f', 1);
        QString progressText;
        if (header_height > count) {
            progressText = tr("%n block(s) remaining (%1% done)", "", header_height - count).arg(percent_done);
        } else {
            // Header tip not yet known to be ahead of the validated tip, so a
            // block count would be misleading; show only the percentage.
            progressText = tr("%1% done").arg(percent_done);
        }

        progressBarLabel->setVisible(true);
        progressBar->setFormat(progressText);
        progressBar->setMaximum(1000000000);
        progressBar->setValue(nVerificationProgress * 1000000000.0 + 0.5);
        progressBar->setVisible(true);

        tooltip = tr("Catching up…") + QString("<br>") + tooltip;
        if(count != prevBlocks)
        {
            labelBlocksIcon->setThemedPixmap(
                QString(":/animation/spinner-%1").arg(spinnerFrame, 3, 10, QChar('0')),
                STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
            spinnerFrame = (spinnerFrame + 1) % SPINNER_FRAMES;
        }
        prevBlocks = count;

#ifdef ENABLE_WALLET
        if(walletFrame)
        {
            walletFrame->showOutOfSyncWarning(true);
            modalOverlay->showHide();
        }
#endif // ENABLE_WALLET

        tooltip += QString("<br>");
        tooltip += tr("Last received block was generated %1 ago.").arg(timeBehindText);
        tooltip += QString("<br>");
        tooltip += tr("Transactions after this will not yet be visible.");
    }

    // Don't word-wrap this (fixed-width) tooltip
    tooltip = QString("<nobr>") + tooltip + QString("</nobr>");

    labelBlocksIcon->setToolTip(tooltip);
    progressBarLabel->setToolTip(tooltip);
    progressBar->setToolTip(tooltip);
}

void BitcoinGUI::message(const QString& title, QString message, unsigned int style, bool* ret, const QString& detailed_message)
{
    // Default title. On macOS, the window title is ignored (as required by the macOS Guidelines).
    QString strTitle{PACKAGE_NAME};
    // Default to information icon
    int nMBoxIcon = QMessageBox::Information;
    int nNotifyIcon = Notificator::Information;

    QString msgType;
    if (!title.isEmpty()) {
        msgType = title;
    } else {
        switch (style) {
        case CClientUIInterface::MSG_ERROR:
            msgType = tr("Error");
            message = tr("Error: %1").arg(message);
            break;
        case CClientUIInterface::MSG_WARNING:
            msgType = tr("Warning");
            message = tr("Warning: %1").arg(message);
            break;
        case CClientUIInterface::MSG_INFORMATION:
            msgType = tr("Information");
            // No need to prepend the prefix here.
            break;
        default:
            break;
        }
    }

    if (!msgType.isEmpty()) {
        strTitle += " - " + msgType;
    }

    if (style & CClientUIInterface::ICON_ERROR) {
        nMBoxIcon = QMessageBox::Critical;
        nNotifyIcon = Notificator::Critical;
    } else if (style & CClientUIInterface::ICON_WARNING) {
        nMBoxIcon = QMessageBox::Warning;
        nNotifyIcon = Notificator::Warning;
    }

    if (style & CClientUIInterface::MODAL) {
        // Check for buttons, use OK as default, if none was supplied
        QMessageBox::StandardButton buttons;
        if (!(buttons = (QMessageBox::StandardButton)(style & CClientUIInterface::BTN_MASK)))
            buttons = QMessageBox::Ok;

        showNormalIfMinimized();
        QMessageBox mBox(static_cast<QMessageBox::Icon>(nMBoxIcon), strTitle, message, buttons, this);
        mBox.setTextFormat(Qt::PlainText);
        mBox.setDetailedText(detailed_message);
        int r = mBox.exec();
        if (ret != nullptr)
            *ret = r == QMessageBox::Ok;
    } else {
        notificator->notify(static_cast<Notificator::Class>(nNotifyIcon), strTitle, message);
    }
}

void BitcoinGUI::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange) {
        overviewAction->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/overview")));
        sendCoinsAction->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/send")));
        receiveCoinsAction->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/receiving_addresses")));
        historyAction->setIcon(platformStyle->SingleColorIcon(QStringLiteral(":/icons/history")));
    }

    QMainWindow::changeEvent(e);

#ifndef Q_OS_MAC // Ignored on Mac
    if(e->type() == QEvent::WindowStateChange)
    {
        if(clientModel && clientModel->getOptionsModel() && clientModel->getOptionsModel()->getMinimizeToTray())
        {
            QWindowStateChangeEvent *wsevt = static_cast<QWindowStateChangeEvent*>(e);
            if(!(wsevt->oldState() & Qt::WindowMinimized) && isMinimized())
            {
                QTimer::singleShot(0, this, &BitcoinGUI::hide);
                e->ignore();
            }
            else if((wsevt->oldState() & Qt::WindowMinimized) && !isMinimized())
            {
                QTimer::singleShot(0, this, &BitcoinGUI::show);
                e->ignore();
            }
        }
    }
#endif
}

void BitcoinGUI::closeEvent(QCloseEvent *event)
{
#ifndef Q_OS_MAC // Ignored on Mac
    if(clientModel && clientModel->getOptionsModel())
    {
        if(!clientModel->getOptionsModel()->getMinimizeOnClose())
        {
            // close rpcConsole in case it was open to make some space for the shutdown window
            rpcConsole->close();

            Q_EMIT quitRequested();
        }
        else
        {
            QMainWindow::showMinimized();
            event->ignore();
        }
    }
#else
    QMainWindow::closeEvent(event);
#endif
}

void BitcoinGUI::showEvent(QShowEvent *event)
{
    // enable the debug window when the main window shows up
    openRPCConsoleAction->setEnabled(true);
    aboutAction->setEnabled(true);
    optionsAction->setEnabled(true);
}

#ifdef ENABLE_WALLET
void BitcoinGUI::incomingTransaction(const QString& date, const QString& assetamount_str, const QString& type, const QString& address, const QString& label, const QString& walletName)
{
    // On new transaction, make an info balloon
    QString msg = tr("Date: %1\n").arg(date) +
                  tr("Amount: %1\n").arg(assetamount_str);
    if (m_node.walletLoader().getWallets().size() > 1 && !walletName.isEmpty()) {
        msg += tr("Wallet: %1\n").arg(walletName);
    }
    msg += tr("Type: %1\n").arg(type);
    if (!label.isEmpty())
        msg += tr("Label: %1\n").arg(label);
    else if (!address.isEmpty())
        msg += tr("Address: %1\n").arg(address);
    message(assetamount_str.startsWith("-") ? tr("Sent transaction") : tr("Incoming transaction"),
             msg, CClientUIInterface::MSG_INFORMATION);
}
#endif // ENABLE_WALLET

void BitcoinGUI::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept only URIs
    if(event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

bool BitcoinGUI::eventFilter(QObject *object, QEvent *event)
{
    // Catch status tip events
    if (event->type() == QEvent::StatusTip)
    {
        // Prevent adding text from setStatusTip(), if we currently use the status bar for displaying other stuff
        if (progressBarLabel->isVisible() || progressBar->isVisible())
            return true;
    }
    return QMainWindow::eventFilter(object, event);
}

#ifdef ENABLE_WALLET
bool BitcoinGUI::handlePaymentRequest(const SendCoinsRecipient& recipient)
{
    // URI has to be valid
    if (walletFrame && walletFrame->handlePaymentRequest(recipient))
    {
        showNormalIfMinimized();
        gotoSendCoinsPage();
        return true;
    }
    return false;
}

void BitcoinGUI::setHDStatus(bool privkeyDisabled, int hdEnabled)
{
    labelWalletHDStatusIcon->setThemedPixmap(privkeyDisabled ? QStringLiteral(":/icons/eye") : hdEnabled ? QStringLiteral(":/icons/hd_enabled") : QStringLiteral(":/icons/hd_disabled"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
    labelWalletHDStatusIcon->setToolTip(privkeyDisabled ? tr("Private key <b>disabled</b>") : hdEnabled ? tr("HD key generation is <b>enabled</b>") : tr("HD key generation is <b>disabled</b>"));
    labelWalletHDStatusIcon->show();
}

void BitcoinGUI::setEncryptionStatus(int status)
{
    switch(status)
    {
    case WalletModel::NoKeys:
        labelWalletEncryptionIcon->hide();
        encryptWalletAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        encryptWalletAction->setEnabled(false);
        break;
    case WalletModel::Unencrypted:
        labelWalletEncryptionIcon->hide();
        encryptWalletAction->setChecked(false);
        changePassphraseAction->setEnabled(false);
        encryptWalletAction->setEnabled(true);
        break;
    case WalletModel::Unlocked:
        labelWalletEncryptionIcon->show();
        labelWalletEncryptionIcon->setThemedPixmap(QStringLiteral(":/icons/lock_open"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
        labelWalletEncryptionIcon->setToolTip(tr("Wallet is <b>encrypted</b> and currently <b>unlocked</b>"));
        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptWalletAction->setEnabled(false);
        break;
    case WalletModel::Locked:
        labelWalletEncryptionIcon->show();
        labelWalletEncryptionIcon->setThemedPixmap(QStringLiteral(":/icons/lock_closed"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
        labelWalletEncryptionIcon->setToolTip(tr("Wallet is <b>encrypted</b> and currently <b>locked</b>"));
        encryptWalletAction->setChecked(true);
        changePassphraseAction->setEnabled(true);
        encryptWalletAction->setEnabled(false);
        break;
    }
}

void BitcoinGUI::updateWalletStatus()
{
    assert(walletFrame);

    WalletView * const walletView = walletFrame->currentWalletView();
    if (!walletView) {
        return;
    }
    WalletModel * const walletModel = walletView->getWalletModel();
    setEncryptionStatus(walletModel->getEncryptionStatus());
    setHDStatus(walletModel->wallet().privateKeysDisabled(), walletModel->wallet().hdEnabled());
}
#endif // ENABLE_WALLET

void BitcoinGUI::updateProxyIcon()
{
    std::string ip_port;
    bool proxy_enabled = clientModel->getProxyInfo(ip_port);

    if (proxy_enabled) {
        if (!GUIUtil::HasPixmap(labelProxyIcon)) {
            QString ip_port_q = QString::fromStdString(ip_port);
            labelProxyIcon->setThemedPixmap((":/icons/proxy"), STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE);
            labelProxyIcon->setToolTip(tr("Proxy is <b>enabled</b>: %1").arg(ip_port_q));
        } else {
            labelProxyIcon->show();
        }
    } else {
        labelProxyIcon->hide();
    }
}

void BitcoinGUI::updateWindowTitle()
{
    QString window_title = PACKAGE_NAME;
#ifdef ENABLE_WALLET
    if (walletFrame) {
        WalletModel* const wallet_model = walletFrame->currentWalletModel();
        if (wallet_model && !wallet_model->getWalletName().isEmpty()) {
            window_title += " - " + wallet_model->getDisplayName();
        }
    }
#endif
    if (!m_network_style->getTitleAddText().isEmpty()) {
        window_title += " - " + m_network_style->getTitleAddText();
    }
    setWindowTitle(window_title);
}

void BitcoinGUI::showNormalIfMinimized(bool fToggleHidden)
{
    if(!clientModel)
        return;

    if (!isHidden() && !isMinimized() && !GUIUtil::isObscured(this) && fToggleHidden) {
        hide();
    } else {
        GUIUtil::bringToFront(this);
    }
}

void BitcoinGUI::toggleHidden()
{
    showNormalIfMinimized(true);
}

void BitcoinGUI::detectShutdown()
{
    if (m_node.shutdownRequested())
    {
        if(rpcConsole)
            rpcConsole->hide();
        Q_EMIT quitRequested();
    }
}

void BitcoinGUI::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0) {
        progressDialog = new QProgressDialog(title, QString(), 0, 100);
        GUIUtil::PolishProgressDialog(progressDialog);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    } else if (nProgress == 100) {
        if (progressDialog) {
            progressDialog->close();
            progressDialog->deleteLater();
            progressDialog = nullptr;
        }
    } else if (progressDialog) {
        progressDialog->setValue(nProgress);
    }
}

void BitcoinGUI::showModalOverlay()
{
    if (modalOverlay && (progressBar->isVisible() || modalOverlay->isLayerVisible()))
        modalOverlay->toggleVisibility();
}

static bool ThreadSafeMessageBox(BitcoinGUI* gui, const bilingual_str& message, const std::string& caption, unsigned int style)
{
    bool modal = (style & CClientUIInterface::MODAL);
    // The SECURE flag has no effect in the Qt GUI.
    // bool secure = (style & CClientUIInterface::SECURE);
    style &= ~CClientUIInterface::SECURE;
    bool ret = false;

    QString detailed_message; // This is original message, in English, for googling and referencing.
    if (message.original != message.translated) {
        detailed_message = BitcoinGUI::tr("Original message:") + "\n" + QString::fromStdString(message.original);
    }

    // In case of modal message, use blocking connection to wait for user to click a button
    bool invoked = QMetaObject::invokeMethod(gui, "message",
                               modal ? GUIUtil::blockingGUIThreadConnection() : Qt::QueuedConnection,
                               Q_ARG(QString, QString::fromStdString(caption)),
                               Q_ARG(QString, QString::fromStdString(message.translated)),
                               Q_ARG(unsigned int, style),
                               Q_ARG(bool*, &ret),
                               Q_ARG(QString, detailed_message));
    assert(invoked);
    return ret;
}

void BitcoinGUI::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_message_box = m_node.handleMessageBox(std::bind(ThreadSafeMessageBox, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    m_handler_question = m_node.handleQuestion(std::bind(ThreadSafeMessageBox, this, std::placeholders::_1, std::placeholders::_3, std::placeholders::_4));
}

void BitcoinGUI::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_message_box->disconnect();
    m_handler_question->disconnect();
}

bool BitcoinGUI::isPrivacyModeActivated() const
{
    assert(m_mask_values_action);
    return m_mask_values_action->isChecked();
}

// ---- SEQUENTIA reference-currency status-bar control ----

ReferenceCurrencyStatusBarControl::ReferenceCurrencyStatusBarControl(const PlatformStyle *platformStyle)
    : optionsModel(nullptr),
      m_platform_style{platformStyle}
{
    setToolTip(tr("Reference currency for the \xE2\x89\x88 valuation."));
    setFocusPolicy(Qt::StrongFocus);
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    addItem(QStringLiteral("USD"));
    connect(this, qOverload<int>(&QComboBox::activated), this, &ReferenceCurrencyStatusBarControl::onActivated);
}

// (Re)build the list in the canonical order (matching the web explorer): BTC, USD,
// SEQ first (always shown), then every other currently-priced asset ticker (WBTC shown
// as BTC) ALPHABETICALLY, always including the current choice. Guarded so programmatic
// changes don't fire onActivated.
void ReferenceCurrencyStatusBarControl::rebuild()
{
    m_updating = true;
    const QString cur = optionsModel ? optionsModel->getReferenceCurrency() : QStringLiteral("USD");
    QStringList head; head << QStringLiteral("BTC") << QStringLiteral("USD") << QStringLiteral("SEQ");
    QStringList rest;
    for (const auto& it : GetReferencePrices()) {
        QString t = QString::fromStdString(it.first).toUpper();
        if (t == QLatin1String("WBTC")) t = QStringLiteral("BTC");
        if (!head.contains(t) && !rest.contains(t)) rest << t;
    }
    if (!cur.isEmpty() && !head.contains(cur) && !rest.contains(cur)) rest << cur;
    rest.sort();
    QStringList opts = head; opts << rest;
    clear();
    addItems(opts);
    const int idx = findText(cur.isEmpty() ? QStringLiteral("USD") : cur);
    if (idx >= 0) setCurrentIndex(idx);
    m_updating = false;
}

void ReferenceCurrencyStatusBarControl::showPopup()
{
    rebuild(); // pick up late-arriving prices before the user sees the list
    QComboBox::showPopup();
}

void ReferenceCurrencyStatusBarControl::setOptionsModel(OptionsModel *_optionsModel)
{
    if (_optionsModel)
    {
        this->optionsModel = _optionsModel;
        connect(_optionsModel, &OptionsModel::referenceCurrencyChanged, this, &ReferenceCurrencyStatusBarControl::updateReferenceCurrency);
        rebuild();
    }
}

void ReferenceCurrencyStatusBarControl::updateReferenceCurrency(const QString& ticker)
{
    if (m_updating) return;
    const int idx = findText(ticker.isEmpty() ? QStringLiteral("USD") : ticker);
    if (idx >= 0) { m_updating = true; setCurrentIndex(idx); m_updating = false; }
    else rebuild();
}

void ReferenceCurrencyStatusBarControl::onActivated(int index)
{
    if (m_updating || !optionsModel) return;
    optionsModel->setReferenceCurrency(itemText(index));
}
