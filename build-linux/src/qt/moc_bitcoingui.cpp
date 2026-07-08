/****************************************************************************
** Meta object code from reading C++ file 'bitcoingui.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../src/qt/bitcoingui.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'bitcoingui.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_BitcoinGUI_t {
    QByteArrayData data[68];
    char stringdata0[935];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_BitcoinGUI_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_BitcoinGUI_t qt_meta_stringdata_BitcoinGUI = {
    {
QT_MOC_LITERAL(0, 0, 10), // "BitcoinGUI"
QT_MOC_LITERAL(1, 11, 13), // "quitRequested"
QT_MOC_LITERAL(2, 25, 0), // ""
QT_MOC_LITERAL(3, 26, 11), // "receivedURI"
QT_MOC_LITERAL(4, 38, 3), // "uri"
QT_MOC_LITERAL(5, 42, 12), // "consoleShown"
QT_MOC_LITERAL(6, 55, 11), // "RPCConsole*"
QT_MOC_LITERAL(7, 67, 7), // "console"
QT_MOC_LITERAL(8, 75, 10), // "setPrivacy"
QT_MOC_LITERAL(9, 86, 7), // "privacy"
QT_MOC_LITERAL(10, 94, 17), // "setNumConnections"
QT_MOC_LITERAL(11, 112, 5), // "count"
QT_MOC_LITERAL(12, 118, 16), // "setNetworkActive"
QT_MOC_LITERAL(13, 135, 14), // "network_active"
QT_MOC_LITERAL(14, 150, 12), // "setNumBlocks"
QT_MOC_LITERAL(15, 163, 9), // "blockDate"
QT_MOC_LITERAL(16, 173, 21), // "nVerificationProgress"
QT_MOC_LITERAL(17, 195, 7), // "headers"
QT_MOC_LITERAL(18, 203, 20), // "SynchronizationState"
QT_MOC_LITERAL(19, 224, 10), // "sync_state"
QT_MOC_LITERAL(20, 235, 7), // "message"
QT_MOC_LITERAL(21, 243, 5), // "title"
QT_MOC_LITERAL(22, 249, 5), // "style"
QT_MOC_LITERAL(23, 255, 5), // "bool*"
QT_MOC_LITERAL(24, 261, 3), // "ret"
QT_MOC_LITERAL(25, 265, 16), // "detailed_message"
QT_MOC_LITERAL(26, 282, 16), // "setCurrentWallet"
QT_MOC_LITERAL(27, 299, 12), // "WalletModel*"
QT_MOC_LITERAL(28, 312, 12), // "wallet_model"
QT_MOC_LITERAL(29, 325, 31), // "setCurrentWalletBySelectorIndex"
QT_MOC_LITERAL(30, 357, 5), // "index"
QT_MOC_LITERAL(31, 363, 18), // "updateWalletStatus"
QT_MOC_LITERAL(32, 382, 20), // "handlePaymentRequest"
QT_MOC_LITERAL(33, 403, 18), // "SendCoinsRecipient"
QT_MOC_LITERAL(34, 422, 9), // "recipient"
QT_MOC_LITERAL(35, 432, 19), // "incomingTransaction"
QT_MOC_LITERAL(36, 452, 4), // "date"
QT_MOC_LITERAL(37, 457, 15), // "assetamount_str"
QT_MOC_LITERAL(38, 473, 4), // "type"
QT_MOC_LITERAL(39, 478, 7), // "address"
QT_MOC_LITERAL(40, 486, 5), // "label"
QT_MOC_LITERAL(41, 492, 10), // "walletName"
QT_MOC_LITERAL(42, 503, 16), // "gotoOverviewPage"
QT_MOC_LITERAL(43, 520, 15), // "gotoHistoryPage"
QT_MOC_LITERAL(44, 536, 20), // "gotoReceiveCoinsPage"
QT_MOC_LITERAL(45, 557, 14), // "gotoAssetsPage"
QT_MOC_LITERAL(46, 572, 15), // "gotoStakingPage"
QT_MOC_LITERAL(47, 588, 19), // "gotoFeePolicyDialog"
QT_MOC_LITERAL(48, 608, 17), // "launchPriceServer"
QT_MOC_LITERAL(49, 626, 15), // "stopPriceServer"
QT_MOC_LITERAL(50, 642, 17), // "gotoSendCoinsPage"
QT_MOC_LITERAL(51, 660, 4), // "addr"
QT_MOC_LITERAL(52, 665, 18), // "gotoSignMessageTab"
QT_MOC_LITERAL(53, 684, 20), // "gotoVerifyMessageTab"
QT_MOC_LITERAL(54, 705, 12), // "gotoLoadPSBT"
QT_MOC_LITERAL(55, 718, 14), // "from_clipboard"
QT_MOC_LITERAL(56, 733, 14), // "optionsClicked"
QT_MOC_LITERAL(57, 748, 12), // "aboutClicked"
QT_MOC_LITERAL(58, 761, 15), // "showDebugWindow"
QT_MOC_LITERAL(59, 777, 30), // "showDebugWindowActivateConsole"
QT_MOC_LITERAL(60, 808, 22), // "showHelpMessageClicked"
QT_MOC_LITERAL(61, 831, 21), // "showNormalIfMinimized"
QT_MOC_LITERAL(62, 853, 13), // "fToggleHidden"
QT_MOC_LITERAL(63, 867, 12), // "toggleHidden"
QT_MOC_LITERAL(64, 880, 14), // "detectShutdown"
QT_MOC_LITERAL(65, 895, 12), // "showProgress"
QT_MOC_LITERAL(66, 908, 9), // "nProgress"
QT_MOC_LITERAL(67, 918, 16) // "showModalOverlay"

    },
    "BitcoinGUI\0quitRequested\0\0receivedURI\0"
    "uri\0consoleShown\0RPCConsole*\0console\0"
    "setPrivacy\0privacy\0setNumConnections\0"
    "count\0setNetworkActive\0network_active\0"
    "setNumBlocks\0blockDate\0nVerificationProgress\0"
    "headers\0SynchronizationState\0sync_state\0"
    "message\0title\0style\0bool*\0ret\0"
    "detailed_message\0setCurrentWallet\0"
    "WalletModel*\0wallet_model\0"
    "setCurrentWalletBySelectorIndex\0index\0"
    "updateWalletStatus\0handlePaymentRequest\0"
    "SendCoinsRecipient\0recipient\0"
    "incomingTransaction\0date\0assetamount_str\0"
    "type\0address\0label\0walletName\0"
    "gotoOverviewPage\0gotoHistoryPage\0"
    "gotoReceiveCoinsPage\0gotoAssetsPage\0"
    "gotoStakingPage\0gotoFeePolicyDialog\0"
    "launchPriceServer\0stopPriceServer\0"
    "gotoSendCoinsPage\0addr\0gotoSignMessageTab\0"
    "gotoVerifyMessageTab\0gotoLoadPSBT\0"
    "from_clipboard\0optionsClicked\0"
    "aboutClicked\0showDebugWindow\0"
    "showDebugWindowActivateConsole\0"
    "showHelpMessageClicked\0showNormalIfMinimized\0"
    "fToggleHidden\0toggleHidden\0detectShutdown\0"
    "showProgress\0nProgress\0showModalOverlay"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_BitcoinGUI[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      42,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,  224,    2, 0x06 /* Public */,
       3,    1,  225,    2, 0x06 /* Public */,
       5,    1,  228,    2, 0x06 /* Public */,
       8,    1,  231,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      10,    1,  234,    2, 0x0a /* Public */,
      12,    1,  237,    2, 0x0a /* Public */,
      14,    5,  240,    2, 0x0a /* Public */,
      20,    5,  251,    2, 0x0a /* Public */,
      20,    4,  262,    2, 0x2a /* Public | MethodCloned */,
      20,    3,  271,    2, 0x2a /* Public | MethodCloned */,
      26,    1,  278,    2, 0x0a /* Public */,
      29,    1,  281,    2, 0x0a /* Public */,
      31,    0,  284,    2, 0x0a /* Public */,
      32,    1,  285,    2, 0x0a /* Public */,
      35,    6,  288,    2, 0x0a /* Public */,
      42,    0,  301,    2, 0x0a /* Public */,
      43,    0,  302,    2, 0x0a /* Public */,
      44,    0,  303,    2, 0x0a /* Public */,
      45,    0,  304,    2, 0x0a /* Public */,
      46,    0,  305,    2, 0x0a /* Public */,
      47,    0,  306,    2, 0x0a /* Public */,
      48,    0,  307,    2, 0x0a /* Public */,
      49,    0,  308,    2, 0x0a /* Public */,
      50,    1,  309,    2, 0x0a /* Public */,
      50,    0,  312,    2, 0x2a /* Public | MethodCloned */,
      52,    1,  313,    2, 0x0a /* Public */,
      52,    0,  316,    2, 0x2a /* Public | MethodCloned */,
      53,    1,  317,    2, 0x0a /* Public */,
      53,    0,  320,    2, 0x2a /* Public | MethodCloned */,
      54,    1,  321,    2, 0x0a /* Public */,
      54,    0,  324,    2, 0x2a /* Public | MethodCloned */,
      56,    0,  325,    2, 0x0a /* Public */,
      57,    0,  326,    2, 0x0a /* Public */,
      58,    0,  327,    2, 0x0a /* Public */,
      59,    0,  328,    2, 0x0a /* Public */,
      60,    0,  329,    2, 0x0a /* Public */,
      61,    0,  330,    2, 0x0a /* Public */,
      61,    1,  331,    2, 0x0a /* Public */,
      63,    0,  334,    2, 0x0a /* Public */,
      64,    0,  335,    2, 0x0a /* Public */,
      65,    2,  336,    2, 0x0a /* Public */,
      67,    0,  341,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, QMetaType::Bool,    9,

 // slots: parameters
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void, QMetaType::Bool,   13,
    QMetaType::Void, QMetaType::Int, QMetaType::QDateTime, QMetaType::Double, QMetaType::Bool, 0x80000000 | 18,   11,   15,   16,   17,   19,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::UInt, 0x80000000 | 23, QMetaType::QString,   21,   20,   22,   24,   25,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::UInt, 0x80000000 | 23,   21,   20,   22,   24,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::UInt,   21,   20,   22,
    QMetaType::Void, 0x80000000 | 27,   28,
    QMetaType::Void, QMetaType::Int,   30,
    QMetaType::Void,
    QMetaType::Bool, 0x80000000 | 33,   34,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QString,   36,   37,   38,   39,   40,   41,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   51,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   51,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   51,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   55,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   62,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::Int,   21,   66,
    QMetaType::Void,

       0        // eod
};

void BitcoinGUI::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<BitcoinGUI *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->quitRequested(); break;
        case 1: _t->receivedURI((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->consoleShown((*reinterpret_cast< RPCConsole*(*)>(_a[1]))); break;
        case 3: _t->setPrivacy((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 4: _t->setNumConnections((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 5: _t->setNetworkActive((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 6: _t->setNumBlocks((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QDateTime(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4])),(*reinterpret_cast< SynchronizationState(*)>(_a[5]))); break;
        case 7: _t->message((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< uint(*)>(_a[3])),(*reinterpret_cast< bool*(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5]))); break;
        case 8: _t->message((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< uint(*)>(_a[3])),(*reinterpret_cast< bool*(*)>(_a[4]))); break;
        case 9: _t->message((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< uint(*)>(_a[3]))); break;
        case 10: _t->setCurrentWallet((*reinterpret_cast< WalletModel*(*)>(_a[1]))); break;
        case 11: _t->setCurrentWalletBySelectorIndex((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 12: _t->updateWalletStatus(); break;
        case 13: { bool _r = _t->handlePaymentRequest((*reinterpret_cast< const SendCoinsRecipient(*)>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 14: _t->incomingTransaction((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5])),(*reinterpret_cast< const QString(*)>(_a[6]))); break;
        case 15: _t->gotoOverviewPage(); break;
        case 16: _t->gotoHistoryPage(); break;
        case 17: _t->gotoReceiveCoinsPage(); break;
        case 18: _t->gotoAssetsPage(); break;
        case 19: _t->gotoStakingPage(); break;
        case 20: _t->gotoFeePolicyDialog(); break;
        case 21: _t->launchPriceServer(); break;
        case 22: _t->stopPriceServer(); break;
        case 23: _t->gotoSendCoinsPage((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 24: _t->gotoSendCoinsPage(); break;
        case 25: _t->gotoSignMessageTab((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 26: _t->gotoSignMessageTab(); break;
        case 27: _t->gotoVerifyMessageTab((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 28: _t->gotoVerifyMessageTab(); break;
        case 29: _t->gotoLoadPSBT((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 30: _t->gotoLoadPSBT(); break;
        case 31: _t->optionsClicked(); break;
        case 32: _t->aboutClicked(); break;
        case 33: _t->showDebugWindow(); break;
        case 34: _t->showDebugWindowActivateConsole(); break;
        case 35: _t->showHelpMessageClicked(); break;
        case 36: _t->showNormalIfMinimized(); break;
        case 37: _t->showNormalIfMinimized((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 38: _t->toggleHidden(); break;
        case 39: _t->detectShutdown(); break;
        case 40: _t->showProgress((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 41: _t->showModalOverlay(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (BitcoinGUI::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&BitcoinGUI::quitRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (BitcoinGUI::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&BitcoinGUI::receivedURI)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (BitcoinGUI::*)(RPCConsole * );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&BitcoinGUI::consoleShown)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (BitcoinGUI::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&BitcoinGUI::setPrivacy)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject BitcoinGUI::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_BitcoinGUI.data,
    qt_meta_data_BitcoinGUI,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *BitcoinGUI::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *BitcoinGUI::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_BitcoinGUI.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int BitcoinGUI::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 42)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 42;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 42)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 42;
    }
    return _id;
}

// SIGNAL 0
void BitcoinGUI::quitRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void BitcoinGUI::receivedURI(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void BitcoinGUI::consoleShown(RPCConsole * _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void BitcoinGUI::setPrivacy(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
struct qt_meta_stringdata_ReferenceCurrencyStatusBarControl_t {
    QByteArrayData data[6];
    char stringdata0[84];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ReferenceCurrencyStatusBarControl_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ReferenceCurrencyStatusBarControl_t qt_meta_stringdata_ReferenceCurrencyStatusBarControl = {
    {
QT_MOC_LITERAL(0, 0, 33), // "ReferenceCurrencyStatusBarCon..."
QT_MOC_LITERAL(1, 34, 23), // "updateReferenceCurrency"
QT_MOC_LITERAL(2, 58, 0), // ""
QT_MOC_LITERAL(3, 59, 6), // "ticker"
QT_MOC_LITERAL(4, 66, 11), // "onActivated"
QT_MOC_LITERAL(5, 78, 5) // "index"

    },
    "ReferenceCurrencyStatusBarControl\0"
    "updateReferenceCurrency\0\0ticker\0"
    "onActivated\0index"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ReferenceCurrencyStatusBarControl[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       2,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    1,   24,    2, 0x08 /* Private */,
       4,    1,   27,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Int,    5,

       0        // eod
};

void ReferenceCurrencyStatusBarControl::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ReferenceCurrencyStatusBarControl *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->updateReferenceCurrency((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->onActivated((*reinterpret_cast< int(*)>(_a[1]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ReferenceCurrencyStatusBarControl::staticMetaObject = { {
    QMetaObject::SuperData::link<QComboBox::staticMetaObject>(),
    qt_meta_stringdata_ReferenceCurrencyStatusBarControl.data,
    qt_meta_data_ReferenceCurrencyStatusBarControl,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ReferenceCurrencyStatusBarControl::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ReferenceCurrencyStatusBarControl::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ReferenceCurrencyStatusBarControl.stringdata0))
        return static_cast<void*>(this);
    return QComboBox::qt_metacast(_clname);
}

int ReferenceCurrencyStatusBarControl::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QComboBox::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 2;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
