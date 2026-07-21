// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUIUTIL_H
#define BITCOIN_QT_GUIUTIL_H

#include <consensus/amount.h>
#include <fs.h>
#include <net.h>
#include <qt/bitcoinunits.h>
#include <asset.h>
#include <netaddress.h>
#include <util/check.h>

#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QItemDelegate>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QObject>
#include <QProgressBar>
#include <QString>
#include <QTableView>

#include <cassert>
#include <chrono>
#include <utility>

class PlatformStyle;
class QValidatedLineEdit;
class SendCoinsRecipient;

namespace interfaces
{
    class Node;
}

QT_BEGIN_NAMESPACE
class QAbstractButton;
class QAbstractItemView;
class QAction;
class QDateTime;
class QDialog;
class QFont;
class QKeySequence;
class QLineEdit;
class QMenu;
class QPoint;
class QProgressDialog;
class QUrl;
class QWidget;
QT_END_NAMESPACE

/** Utility functions used by the Bitcoin Qt UI.
 */
namespace GUIUtil
{
    // Use this flags to prevent a "What's This" button in the title bar of the dialog on Windows.
    constexpr auto dialog_flags = Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint;

    // Create human-readable string from date
    QString dateTimeStr(const QDateTime &datetime);
    QString dateTimeStr(qint64 nTime);

    // Return a monospace font
    QFont fixedPitchFont(bool use_embedded_font = false);

    // Set up widget for address
    void setupAddressWidget(QValidatedLineEdit *widget, QWidget *parent);

    /**
     * Connects an additional shortcut to a QAbstractButton. Works around the
     * one shortcut limitation of the button's shortcut property.
     * @param[in] button    QAbstractButton to assign shortcut to
     * @param[in] shortcut  QKeySequence to use as shortcut
     */
    void AddButtonShortcut(QAbstractButton* button, const QKeySequence& shortcut);

    /**
     * Robustly restore a top-level window's geometry from a persisted QSettings
     * byte array (as produced by QWidget::saveGeometry()).
     *
     * Unlike a bare QWidget::restoreGeometry(), this also rejects geometries that
     * are well-formed but unusable — degenerate (zero/negative size), larger than
     * any available screen, or entirely off every screen. Such a value (e.g. left
     * behind after a window was accidentally sized to extremes) would otherwise be
     * applied verbatim and can crash on show() when an oversized backing store is
     * created. When the stored value is missing, unreadable, or rejected this
     * returns false and leaves the widget's default geometry untouched, so the
     * caller can fall back to a sane placement (see MoveToScreenCenter()).
     *
     * @param[in] window    top-level widget to restore
     * @param[in] geometry  bytes previously produced by window->saveGeometry()
     * @return true if a valid geometry was restored, false otherwise
     */
    bool RestoreWindowGeometry(QWidget* window, const QByteArray& geometry);

    /**
     * Center a top-level window on the screen it currently belongs to (falling
     * back to the primary screen). Null-safe: does nothing if no screen is
     * available, avoiding a crash from dereferencing a null QScreen.
     */
    void MoveToScreenCenter(QWidget* window);

    // Parse "bitcoin:" URI into recipient object, return true on successful parsing
    bool parseBitcoinURI(const QUrl &uri, SendCoinsRecipient *out);
    bool parseBitcoinURI(QString uri, SendCoinsRecipient *out);
    QString formatBitcoinURI(const SendCoinsRecipient &info);

    // Returns true if given address+amount meets "dust" definition
    bool isDust(interfaces::Node& node, const QString& address, const CAmount& amount);

    // HTML escaping for rich text controls
    QString HtmlEscape(const QString& str, bool fMultiLine=false);
    QString HtmlEscape(const std::string& str, bool fMultiLine=false);

    /** Copy a field of the currently selected entry of a view to the clipboard. Does nothing if nothing
        is selected.
       @param[in] column  Data column to extract from the model
       @param[in] role    Data role to extract from the model
       @see  TransactionView::copyLabel, TransactionView::copyAmount, TransactionView::copyAddress
     */
    void copyEntryData(const QAbstractItemView *view, int column, int role=Qt::EditRole);

    /** Return a field of the currently selected entry as a QString. Does nothing if nothing
        is selected.
       @param[in] column  Data column to extract from the model
       @see  TransactionView::copyLabel, TransactionView::copyAmount, TransactionView::copyAddress
     */
    QList<QModelIndex> getEntryData(const QAbstractItemView *view, int column);

    /** Returns true if the specified field of the currently selected view entry is not empty.
       @param[in] column  Data column to extract from the model
       @param[in] role    Data role to extract from the model
       @see  TransactionView::contextualMenu
     */
    bool hasEntryData(const QAbstractItemView *view, int column, int role);

    void setClipboard(const QString& str);

    /**
     * Loads the font from the file specified by file_name, aborts if it fails.
     */
    void LoadFont(const QString& file_name);

    /**
     * Determine default data directory for operating system.
     */
    QString getDefaultDataDirectory();

    /** Get save filename, mimics QFileDialog::getSaveFileName, except that it appends a default suffix
        when no suffix is provided by the user.

      @param[in] parent  Parent window (or 0)
      @param[in] caption Window caption (or empty, for default)
      @param[in] dir     Starting directory (or empty, to default to documents directory)
      @param[in] filter  Filter specification such as "Comma Separated Files (*.csv)"
      @param[out] selectedSuffixOut  Pointer to return the suffix (file type) that was selected (or 0).
                  Can be useful when choosing the save file format based on suffix.
     */
    QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
        const QString &filter,
        QString *selectedSuffixOut);

    /** Get open filename, convenience wrapper for QFileDialog::getOpenFileName.

      @param[in] parent  Parent window (or 0)
      @param[in] caption Window caption (or empty, for default)
      @param[in] dir     Starting directory (or empty, to default to documents directory)
      @param[in] filter  Filter specification such as "Comma Separated Files (*.csv)"
      @param[out] selectedSuffixOut  Pointer to return the suffix (file type) that was selected (or 0).
                  Can be useful when choosing the save file format based on suffix.
     */
    QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
        const QString &filter,
        QString *selectedSuffixOut);

    /** Get connection type to call object slot in GUI thread with invokeMethod. The call will be blocking.

       @returns If called from the GUI thread, return a Qt::DirectConnection.
                If called from another thread, return a Qt::BlockingQueuedConnection.
    */
    Qt::ConnectionType blockingGUIThreadConnection();

    // Determine whether a widget is hidden behind other windows
    bool isObscured(QWidget *w);

    // Activate, show and raise the widget
    void bringToFront(QWidget* w);

    // Set shortcut to close window
    void handleCloseWindowShortcut(QWidget* w);

    // Open debug.log
    void openDebugLogfile();

    // Open the config file
    bool openBitcoinConf();

    /** Qt event filter that intercepts ToolTipChange events, and replaces the tooltip with a rich text
      representation if needed. This assures that Qt can word-wrap long tooltip messages.
      Tooltips longer than the provided size threshold (in characters) are wrapped.
     */
    class ToolTipToRichTextFilter : public QObject
    {
        Q_OBJECT

    public:
        explicit ToolTipToRichTextFilter(int size_threshold, QObject *parent = nullptr);

    protected:
        bool eventFilter(QObject *obj, QEvent *evt) override;

    private:
        int size_threshold;
    };

    /**
     * Qt event filter that intercepts QEvent::FocusOut events for QLabel objects, and
     * resets their `textInteractionFlags' property to get rid of the visible cursor.
     *
     * This is a temporary fix of QTBUG-59514.
     */
    class LabelOutOfFocusEventFilter : public QObject
    {
        Q_OBJECT

    public:
        explicit LabelOutOfFocusEventFilter(QObject* parent);
        bool eventFilter(QObject* watched, QEvent* event) override;
    };

    bool GetStartOnSystemStartup();
    bool SetStartOnSystemStartup(bool fAutoStart);

    /** Convert QString to OS specific boost path through UTF-8 */
    fs::path QStringToPath(const QString &path);

    /** Convert OS specific boost path to QString through UTF-8 */
    QString PathToQString(const fs::path &path);

    /* User-facing label for an asset: the chain-aware ticker (tSEQ/SEQ) for the policy asset,
       otherwise the asset registry identifier. Avoids the policy asset rendering as "bitcoin"
       (its default pegged-asset name) in selectors and amount labels. */
    QString assetDisplayName(const CAsset& asset);

    /* SEQUENTIA: whether an asset carries a human-readable registry label. False for assets the
       node has never seen registered — their only identity is the 64-hex id, so the UI must show
       (and elide) the id rather than a name. The policy asset (tSEQ/SEQ) is always named. */
    bool assetIsNamed(const CAsset& asset);

    /* Elide a long identifier in the middle for display: "aaaaaaaa…zzzzzzzz". Strings no longer
       than head+tail+1 are returned unchanged. Put the full value in a tooltip. */
    QString ellipsizeMiddle(const QString& text, int head = 8, int tail = 8);

    /* SEQUENTIA: the number of decimal places to display/parse for an asset — the
       on-chain denomination when known, else the registry precision, else 8. The
       policy asset (SEQ) is always 8. */
    int assetPrecision(const CAsset& asset);

    /* Format an amount of assets in a user-friendly style */
    QString formatAssetAmount(const CAsset&, const CAmount&, int bitcoin_unit, BitcoinUnits::SeparatorStyle, bool include_asset_name = true);

    /* Format one or more asset+amounts in a user-friendly style */
    QString formatMultiAssetAmount(const CAmountMap&, int bitcoin_unit, BitcoinUnits::SeparatorStyle, QString line_separator);

    /* SEQUENTIA: as formatMultiAssetAmount, but each asset line gets its own muted
       "≈ <value> <REF>" appended (via formatReferenceApprox). Display-only. */
    QString formatMultiAssetAmountWithValue(const CAmountMap&, int bitcoin_unit, BitcoinUnits::SeparatorStyle, const QString& refTicker, QString line_separator);

    /* SEQUENTIA: a muted "≈ <amount> <REF>" valuing (asset, amount) in the user-chosen reference
       currency, using the node's cached USD price feed. Empty when unpriced/unavailable or when the
       amount is already in the reference denomination. Display-only — never used for copy/export. */
    QString formatReferenceApprox(const CAsset& asset, const CAmount& amount, const QString& refTicker);
    /* SEQUENTIA: as above, summed across a multi-asset map (e.g. a total balance). */
    QString formatMultiAssetReferenceApprox(const CAmountMap& amountmap, const QString& refTicker);
    /* SEQUENTIA: "≈ <ref>" from an asset LABEL/ticker + a whole-unit amount (for RPC-string tables
       like the assets page, where no CAsset/CAmount is available). Empty if unpriced. */
    QString formatReferenceApproxByLabel(const QString& assetLabel, double wholeUnits, const QString& refTicker);

    /* SEQUENTIA: the value of (asset, amount) in the reference currency as a plain number.
       Returns false when either side is unpriced, leaving `out` untouched. Unlike
       formatReferenceApprox this does not suppress the case asset == reference: callers
       that must always show a value (the fee headline) need the number regardless. */
    bool referenceValueOf(const CAsset& asset, const CAmount& amount, const QString& refTicker, double& out);

    /* SEQUENTIA: how many whole units of `to` one whole unit of `from` buys, at feed prices.
       0.0 when either asset is unpriced. Display-only, like every price-feed helper here:
       the node converts fees by its own feed at validation time. */
    double assetExchangeRate(const CAsset& from, const CAsset& to);

    /* SEQUENTIA: convert an atom amount of `from` into the equivalent atom amount of `to`
       at feed prices, honouring each asset's own precision. False when either is unpriced. */
    bool convertAssetAmount(const CAsset& from, const CAmount& amount, const CAsset& to, CAmount& out);

    /* SEQUENTIA: one whole unit of `asset` valued in the reference currency. False if unpriced. */
    bool unitReferenceValue(const CAsset& asset, const QString& refTicker, double& out);

    /* SEQUENTIA: render a reference-currency value as a bare "<number> <REF>" (no "≈"),
       with the decimal count that suits the reference (8 for BTC, else 2/6 by magnitude). */
    QString formatReferenceAmount(double value, const QString& refTicker);

    /* SEQUENTIA: the user's chosen reference currency ticker (QSettings), defaulting to USD. */
    QString referenceCurrency();

    /* SEQUENTIA: locate the price-server sidecar (price_server.py) or one of its siblings
       (pass the file name, e.g. "config.example.json"). Search order: a location the user
       pointed at previously, then a walk up from the binary, then one level into each
       ancestor's subdirectories (which finds a source checkout sitting next to an unpacked
       binary). `searched` collects the directories tried, so a failure can say where it
       looked instead of just "not found". Empty when nothing matched. */
    QString findPriceServerFile(const QString& file_name, QStringList* searched = nullptr);

    /* SEQUENTIA: ask the user to point at price_server.py and remember its directory for
       next time. Returns the chosen path, or empty if they cancelled. */
    QString promptForPriceServerScript(QWidget* parent);

    /* SEQUENTIA: the reference-price feed the node is actually reading (-referencepricesurl),
       or an empty string when none is configured. This is whose prices the wallet shows. */
    QString referencePriceFeedUrl();

    /* SEQUENTIA: a Python interpreter that can actually run the price-server sidecar.
       Prefers one bundled beside the script or the binary, then a real interpreter on
       PATH. On Windows this deliberately skips the "python3" App Execution Alias under
       WindowsApps: it is a Microsoft Store stub that exits with "Python was not found"
       instead of running anything, which QProcess still reports as a successful start.
       Empty when no usable interpreter exists. */
    QString findPythonInterpreter(const QString& scriptDir);

    /* SEQUENTIA: whether the node's cached price feed carries a positive price for this asset.
       Fees paid in an unpriced asset are unlikely to ever be accepted by a block producer, so
       the send dialog uses this to pick a sane default fee asset and to warn about bad picks. */
    bool assetHasMarketPrice(const CAsset& asset);

    /* Parse an amount of a given asset from text */
    bool parseAssetAmount(const CAsset&, const QString& text, int bitcoin_unit, CAmount *val_out);

    /** Convert enum Network to QString */
    QString NetworkToQString(Network net);

    /** Convert enum ConnectionType to QString */
    QString ConnectionTypeToQString(ConnectionType conn_type, bool prepend_direction);

    /** Convert seconds into a QString with days, hours, mins, secs */
    QString formatDurationStr(std::chrono::seconds dur);

    /** Format CNodeStats.nServices bitmask into a user-readable string */
    QString formatServicesStr(quint64 mask);

    /** Format a CNodeStats.m_last_ping_time into a user-readable string or display N/A, if 0 */
    QString formatPingTime(std::chrono::microseconds ping_time);

    /** Format a CNodeCombinedStats.nTimeOffset into a user-readable string */
    QString formatTimeOffset(int64_t nTimeOffset);

    QString formatNiceTimeOffset(qint64 secs);

    QString formatBytes(uint64_t bytes);

    qreal calculateIdealFontSize(int width, const QString& text, QFont font, qreal minPointSize = 4, qreal startPointSize = 14);

    class ThemedLabel : public QLabel
    {
        Q_OBJECT

    public:
        explicit ThemedLabel(const PlatformStyle* platform_style, QWidget* parent = nullptr);
        void setThemedPixmap(const QString& image_filename, int width, int height);

    protected:
        void changeEvent(QEvent* e) override;

    private:
        const PlatformStyle* m_platform_style;
        QString m_image_filename;
        int m_pixmap_width;
        int m_pixmap_height;
        void updateThemedPixmap();
    };

    class ClickableLabel : public ThemedLabel
    {
        Q_OBJECT

    public:
        explicit ClickableLabel(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    Q_SIGNALS:
        /** Emitted when the label is clicked. The relative mouse coordinates of the click are
         * passed to the signal.
         */
        void clicked(const QPoint& point);
    protected:
        void mouseReleaseEvent(QMouseEvent *event) override;
    };

    class ClickableProgressBar : public QProgressBar
    {
        Q_OBJECT

    Q_SIGNALS:
        /** Emitted when the progressbar is clicked. The relative mouse coordinates of the click are
         * passed to the signal.
         */
        void clicked(const QPoint& point);
    protected:
        void mouseReleaseEvent(QMouseEvent *event) override;
    };

    typedef ClickableProgressBar ProgressBar;

    class ItemDelegate : public QItemDelegate
    {
        Q_OBJECT
    public:
        ItemDelegate(QObject* parent) : QItemDelegate(parent) {}

    Q_SIGNALS:
        void keyEscapePressed();

    private:
        bool eventFilter(QObject *object, QEvent *event) override;
    };

    // Fix known bugs in QProgressDialog class.
    void PolishProgressDialog(QProgressDialog* dialog);

    /**
     * Returns the distance in pixels appropriate for drawing a subsequent character after text.
     *
     * In Qt 5.12 and before the QFontMetrics::width() is used and it is deprecated since Qt 5.13.
     * In Qt 5.11 the QFontMetrics::horizontalAdvance() was introduced.
     */
    int TextWidth(const QFontMetrics& fm, const QString& text);

    /**
     * Writes to debug.log short info about the used Qt and the host system.
     */
    void LogQtInfo();

    /**
     * Call QMenu::popup() only on supported QT_QPA_PLATFORM.
     */
    void PopupMenu(QMenu* menu, const QPoint& point, QAction* at_action = nullptr);

    /**
     * Returns the start-moment of the day in local time.
     *
     * QDateTime::QDateTime(const QDate& date) is deprecated since Qt 5.15.
     * QDate::startOfDay() was introduced in Qt 5.14.
     */
    QDateTime StartOfDay(const QDate& date);

    /**
     * Returns true if pixmap has been set.
     *
     * QPixmap* QLabel::pixmap() is deprecated since Qt 5.15.
     */
    bool HasPixmap(const QLabel* label);
    QImage GetImage(const QLabel* label);

    /**
     * Splits the string into substrings wherever separator occurs, and returns
     * the list of those strings. Empty strings do not appear in the result.
     *
     * QString::split() signature differs in different Qt versions:
     *  - QString::SplitBehavior is deprecated since Qt 5.15
     *  - Qt::SplitBehavior was introduced in Qt 5.14
     * If {QString|Qt}::SkipEmptyParts behavior is required, use this
     * function instead of QString::split().
     */
    template <typename SeparatorType>
    QStringList SplitSkipEmptyParts(const QString& string, const SeparatorType& separator)
    {
    #if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        return string.split(separator, Qt::SkipEmptyParts);
    #else
        return string.split(separator, QString::SkipEmptyParts);
    #endif
    }

    /**
     * Queue a function to run in an object's event loop. This can be
     * replaced by a call to the QMetaObject::invokeMethod functor overload after Qt 5.10, but
     * for now use a QObject::connect for compatibility with older Qt versions, based on
     * https://stackoverflow.com/questions/21646467/how-to-execute-a-functor-or-a-lambda-in-a-given-thread-in-qt-gcd-style
     */
    template <typename Fn>
    void ObjectInvoke(QObject* object, Fn&& function, Qt::ConnectionType connection = Qt::QueuedConnection)
    {
        QObject source;
        QObject::connect(&source, &QObject::destroyed, object, std::forward<Fn>(function), connection);
    }

    /**
     * Replaces a plain text link with an HTML tagged one.
     */
    QString MakeHtmlLink(const QString& source, const QString& link);

    void PrintSlotException(
        const std::exception* exception,
        const QObject* sender,
        const QObject* receiver);

    /**
     * A drop-in replacement of QObject::connect function
     * (see: https://doc.qt.io/qt-5/qobject.html#connect-3), that
     * guaranties that all exceptions are handled within the slot.
     *
     * NOTE: This function is incompatible with Qt private signals.
     */
    template <typename Sender, typename Signal, typename Receiver, typename Slot>
    auto ExceptionSafeConnect(
        Sender sender, Signal signal, Receiver receiver, Slot method,
        Qt::ConnectionType type = Qt::AutoConnection)
    {
        return QObject::connect(
            sender, signal, receiver,
            [sender, receiver, method](auto&&... args) {
                bool ok{true};
                try {
                    (receiver->*method)(std::forward<decltype(args)>(args)...);
                } catch (const NonFatalCheckError& e) {
                    PrintSlotException(&e, sender, receiver);
                    ok = QMetaObject::invokeMethod(
                        qApp, "handleNonFatalException",
                        blockingGUIThreadConnection(),
                        Q_ARG(QString, QString::fromStdString(e.what())));
                } catch (const std::exception& e) {
                    PrintSlotException(&e, sender, receiver);
                    ok = QMetaObject::invokeMethod(
                        qApp, "handleRunawayException",
                        blockingGUIThreadConnection(),
                        Q_ARG(QString, QString::fromStdString(e.what())));
                } catch (...) {
                    PrintSlotException(nullptr, sender, receiver);
                    ok = QMetaObject::invokeMethod(
                        qApp, "handleRunawayException",
                        blockingGUIThreadConnection(),
                        Q_ARG(QString, "Unknown failure occurred."));
                }
                assert(ok);
            },
            type);
    }

    /**
     * Shows a QDialog instance asynchronously, and deletes it on close.
     */
    void ShowModalDialogAsynchronously(QDialog* dialog);

    inline bool IsEscapeOrBack(int key)
    {
        if (key == Qt::Key_Escape) return true;
#ifdef Q_OS_ANDROID
        if (key == Qt::Key_Back) return true;
#endif // Q_OS_ANDROID
        return false;
    }

} // namespace GUIUtil

#endif // BITCOIN_QT_GUIUTIL_H
