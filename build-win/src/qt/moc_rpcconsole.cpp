/****************************************************************************
** Meta object code from reading C++ file 'rpcconsole.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../src/qt/rpcconsole.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'rpcconsole.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_RPCConsole_t {
    QByteArrayData data[62];
    char stringdata0[824];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_RPCConsole_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_RPCConsole_t qt_meta_stringdata_RPCConsole = {
    {
QT_MOC_LITERAL(0, 0, 10), // "RPCConsole"
QT_MOC_LITERAL(1, 11, 10), // "cmdRequest"
QT_MOC_LITERAL(2, 22, 0), // ""
QT_MOC_LITERAL(3, 23, 7), // "command"
QT_MOC_LITERAL(4, 31, 18), // "const WalletModel*"
QT_MOC_LITERAL(5, 50, 12), // "wallet_model"
QT_MOC_LITERAL(6, 63, 25), // "on_lineEdit_returnPressed"
QT_MOC_LITERAL(7, 89, 27), // "on_tabWidget_currentChanged"
QT_MOC_LITERAL(8, 117, 5), // "index"
QT_MOC_LITERAL(9, 123, 33), // "on_openDebugLogfileButton_cli..."
QT_MOC_LITERAL(10, 157, 29), // "on_sldGraphRange_valueChanged"
QT_MOC_LITERAL(11, 187, 5), // "value"
QT_MOC_LITERAL(12, 193, 18), // "updateTrafficStats"
QT_MOC_LITERAL(13, 212, 12), // "totalBytesIn"
QT_MOC_LITERAL(14, 225, 13), // "totalBytesOut"
QT_MOC_LITERAL(15, 239, 11), // "resizeEvent"
QT_MOC_LITERAL(16, 251, 13), // "QResizeEvent*"
QT_MOC_LITERAL(17, 265, 5), // "event"
QT_MOC_LITERAL(18, 271, 9), // "showEvent"
QT_MOC_LITERAL(19, 281, 11), // "QShowEvent*"
QT_MOC_LITERAL(20, 293, 9), // "hideEvent"
QT_MOC_LITERAL(21, 303, 11), // "QHideEvent*"
QT_MOC_LITERAL(22, 315, 25), // "showPeersTableContextMenu"
QT_MOC_LITERAL(23, 341, 5), // "point"
QT_MOC_LITERAL(24, 347, 23), // "showBanTableContextMenu"
QT_MOC_LITERAL(25, 371, 28), // "showOrHideBanTableIfRequired"
QT_MOC_LITERAL(26, 400, 17), // "clearSelectedNode"
QT_MOC_LITERAL(27, 418, 18), // "updateDetailWidget"
QT_MOC_LITERAL(28, 437, 5), // "clear"
QT_MOC_LITERAL(29, 443, 11), // "keep_prompt"
QT_MOC_LITERAL(30, 455, 10), // "fontBigger"
QT_MOC_LITERAL(31, 466, 11), // "fontSmaller"
QT_MOC_LITERAL(32, 478, 11), // "setFontSize"
QT_MOC_LITERAL(33, 490, 7), // "newSize"
QT_MOC_LITERAL(34, 498, 7), // "message"
QT_MOC_LITERAL(35, 506, 8), // "category"
QT_MOC_LITERAL(36, 515, 3), // "msg"
QT_MOC_LITERAL(37, 519, 4), // "html"
QT_MOC_LITERAL(38, 524, 17), // "setNumConnections"
QT_MOC_LITERAL(39, 542, 5), // "count"
QT_MOC_LITERAL(40, 548, 16), // "setNetworkActive"
QT_MOC_LITERAL(41, 565, 13), // "networkActive"
QT_MOC_LITERAL(42, 579, 12), // "setNumBlocks"
QT_MOC_LITERAL(43, 592, 9), // "blockDate"
QT_MOC_LITERAL(44, 602, 21), // "nVerificationProgress"
QT_MOC_LITERAL(45, 624, 7), // "headers"
QT_MOC_LITERAL(46, 632, 14), // "setMempoolSize"
QT_MOC_LITERAL(47, 647, 11), // "numberOfTxs"
QT_MOC_LITERAL(48, 659, 6), // "size_t"
QT_MOC_LITERAL(49, 666, 8), // "dynUsage"
QT_MOC_LITERAL(50, 675, 13), // "browseHistory"
QT_MOC_LITERAL(51, 689, 6), // "offset"
QT_MOC_LITERAL(52, 696, 11), // "scrollToEnd"
QT_MOC_LITERAL(53, 708, 22), // "disconnectSelectedNode"
QT_MOC_LITERAL(54, 731, 15), // "banSelectedNode"
QT_MOC_LITERAL(55, 747, 7), // "bantime"
QT_MOC_LITERAL(56, 755, 17), // "unbanSelectedNode"
QT_MOC_LITERAL(57, 773, 11), // "setTabFocus"
QT_MOC_LITERAL(58, 785, 8), // "TabTypes"
QT_MOC_LITERAL(59, 794, 7), // "tabType"
QT_MOC_LITERAL(60, 802, 12), // "updateAlerts"
QT_MOC_LITERAL(61, 815, 8) // "warnings"

    },
    "RPCConsole\0cmdRequest\0\0command\0"
    "const WalletModel*\0wallet_model\0"
    "on_lineEdit_returnPressed\0"
    "on_tabWidget_currentChanged\0index\0"
    "on_openDebugLogfileButton_clicked\0"
    "on_sldGraphRange_valueChanged\0value\0"
    "updateTrafficStats\0totalBytesIn\0"
    "totalBytesOut\0resizeEvent\0QResizeEvent*\0"
    "event\0showEvent\0QShowEvent*\0hideEvent\0"
    "QHideEvent*\0showPeersTableContextMenu\0"
    "point\0showBanTableContextMenu\0"
    "showOrHideBanTableIfRequired\0"
    "clearSelectedNode\0updateDetailWidget\0"
    "clear\0keep_prompt\0fontBigger\0fontSmaller\0"
    "setFontSize\0newSize\0message\0category\0"
    "msg\0html\0setNumConnections\0count\0"
    "setNetworkActive\0networkActive\0"
    "setNumBlocks\0blockDate\0nVerificationProgress\0"
    "headers\0setMempoolSize\0numberOfTxs\0"
    "size_t\0dynUsage\0browseHistory\0offset\0"
    "scrollToEnd\0disconnectSelectedNode\0"
    "banSelectedNode\0bantime\0unbanSelectedNode\0"
    "setTabFocus\0TabTypes\0tabType\0updateAlerts\0"
    "warnings"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_RPCConsole[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      32,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,  174,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       6,    0,  179,    2, 0x08 /* Private */,
       7,    1,  180,    2, 0x08 /* Private */,
       9,    0,  183,    2, 0x08 /* Private */,
      10,    1,  184,    2, 0x08 /* Private */,
      12,    2,  187,    2, 0x08 /* Private */,
      15,    1,  192,    2, 0x08 /* Private */,
      18,    1,  195,    2, 0x08 /* Private */,
      20,    1,  198,    2, 0x08 /* Private */,
      22,    1,  201,    2, 0x08 /* Private */,
      24,    1,  204,    2, 0x08 /* Private */,
      25,    0,  207,    2, 0x08 /* Private */,
      26,    0,  208,    2, 0x08 /* Private */,
      27,    0,  209,    2, 0x08 /* Private */,
      28,    1,  210,    2, 0x0a /* Public */,
      28,    0,  213,    2, 0x2a /* Public | MethodCloned */,
      30,    0,  214,    2, 0x0a /* Public */,
      31,    0,  215,    2, 0x0a /* Public */,
      32,    1,  216,    2, 0x0a /* Public */,
      34,    2,  219,    2, 0x0a /* Public */,
      34,    3,  224,    2, 0x0a /* Public */,
      38,    1,  231,    2, 0x0a /* Public */,
      40,    1,  234,    2, 0x0a /* Public */,
      42,    4,  237,    2, 0x0a /* Public */,
      46,    2,  246,    2, 0x0a /* Public */,
      50,    1,  251,    2, 0x0a /* Public */,
      52,    0,  254,    2, 0x0a /* Public */,
      53,    0,  255,    2, 0x0a /* Public */,
      54,    1,  256,    2, 0x0a /* Public */,
      56,    0,  259,    2, 0x0a /* Public */,
      57,    1,  260,    2, 0x0a /* Public */,
      60,    1,  263,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, 0x80000000 | 4,    3,    5,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,    8,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void, QMetaType::ULongLong, QMetaType::ULongLong,   13,   14,
    QMetaType::Void, 0x80000000 | 16,   17,
    QMetaType::Void, 0x80000000 | 19,   17,
    QMetaType::Void, 0x80000000 | 21,   17,
    QMetaType::Void, QMetaType::QPoint,   23,
    QMetaType::Void, QMetaType::QPoint,   23,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   29,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   33,
    QMetaType::Void, QMetaType::Int, QMetaType::QString,   35,   36,
    QMetaType::Void, QMetaType::Int, QMetaType::QString, QMetaType::Bool,   35,   34,   37,
    QMetaType::Void, QMetaType::Int,   39,
    QMetaType::Void, QMetaType::Bool,   41,
    QMetaType::Void, QMetaType::Int, QMetaType::QDateTime, QMetaType::Double, QMetaType::Bool,   39,   43,   44,   45,
    QMetaType::Void, QMetaType::Long, 0x80000000 | 48,   47,   49,
    QMetaType::Void, QMetaType::Int,   51,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   55,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 58,   59,
    QMetaType::Void, QMetaType::QString,   61,

       0        // eod
};

void RPCConsole::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<RPCConsole *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->cmdRequest((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const WalletModel*(*)>(_a[2]))); break;
        case 1: _t->on_lineEdit_returnPressed(); break;
        case 2: _t->on_tabWidget_currentChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 3: _t->on_openDebugLogfileButton_clicked(); break;
        case 4: _t->on_sldGraphRange_valueChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 5: _t->updateTrafficStats((*reinterpret_cast< quint64(*)>(_a[1])),(*reinterpret_cast< quint64(*)>(_a[2]))); break;
        case 6: _t->resizeEvent((*reinterpret_cast< QResizeEvent*(*)>(_a[1]))); break;
        case 7: _t->showEvent((*reinterpret_cast< QShowEvent*(*)>(_a[1]))); break;
        case 8: _t->hideEvent((*reinterpret_cast< QHideEvent*(*)>(_a[1]))); break;
        case 9: _t->showPeersTableContextMenu((*reinterpret_cast< const QPoint(*)>(_a[1]))); break;
        case 10: _t->showBanTableContextMenu((*reinterpret_cast< const QPoint(*)>(_a[1]))); break;
        case 11: _t->showOrHideBanTableIfRequired(); break;
        case 12: _t->clearSelectedNode(); break;
        case 13: _t->updateDetailWidget(); break;
        case 14: _t->clear((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 15: _t->clear(); break;
        case 16: _t->fontBigger(); break;
        case 17: _t->fontSmaller(); break;
        case 18: _t->setFontSize((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 19: _t->message((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 20: _t->message((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3]))); break;
        case 21: _t->setNumConnections((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 22: _t->setNetworkActive((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 23: _t->setNumBlocks((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QDateTime(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4]))); break;
        case 24: _t->setMempoolSize((*reinterpret_cast< long(*)>(_a[1])),(*reinterpret_cast< size_t(*)>(_a[2]))); break;
        case 25: _t->browseHistory((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 26: _t->scrollToEnd(); break;
        case 27: _t->disconnectSelectedNode(); break;
        case 28: _t->banSelectedNode((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 29: _t->unbanSelectedNode(); break;
        case 30: _t->setTabFocus((*reinterpret_cast< TabTypes(*)>(_a[1]))); break;
        case 31: _t->updateAlerts((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (RPCConsole::*)(const QString & , const WalletModel * );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&RPCConsole::cmdRequest)) {
                *result = 0;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject RPCConsole::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_RPCConsole.data,
    qt_meta_data_RPCConsole,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *RPCConsole::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *RPCConsole::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_RPCConsole.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int RPCConsole::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 32)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 32;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 32)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 32;
    }
    return _id;
}

// SIGNAL 0
void RPCConsole::cmdRequest(const QString & _t1, const WalletModel * _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
