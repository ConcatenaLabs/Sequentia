/****************************************************************************
** Meta object code from reading C++ file 'clientmodel.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../src/qt/clientmodel.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'clientmodel.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ClientModel_t {
    QByteArrayData data[30];
    char stringdata0[397];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ClientModel_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ClientModel_t qt_meta_stringdata_ClientModel = {
    {
QT_MOC_LITERAL(0, 0, 11), // "ClientModel"
QT_MOC_LITERAL(1, 12, 21), // "numConnectionsChanged"
QT_MOC_LITERAL(2, 34, 0), // ""
QT_MOC_LITERAL(3, 35, 5), // "count"
QT_MOC_LITERAL(4, 41, 16), // "numBlocksChanged"
QT_MOC_LITERAL(5, 58, 9), // "blockDate"
QT_MOC_LITERAL(6, 68, 21), // "nVerificationProgress"
QT_MOC_LITERAL(7, 90, 6), // "header"
QT_MOC_LITERAL(8, 97, 20), // "SynchronizationState"
QT_MOC_LITERAL(9, 118, 10), // "sync_state"
QT_MOC_LITERAL(10, 129, 18), // "mempoolSizeChanged"
QT_MOC_LITERAL(11, 148, 6), // "size_t"
QT_MOC_LITERAL(12, 155, 18), // "mempoolSizeInBytes"
QT_MOC_LITERAL(13, 174, 20), // "networkActiveChanged"
QT_MOC_LITERAL(14, 195, 13), // "networkActive"
QT_MOC_LITERAL(15, 209, 13), // "alertsChanged"
QT_MOC_LITERAL(16, 223, 8), // "warnings"
QT_MOC_LITERAL(17, 232, 12), // "bytesChanged"
QT_MOC_LITERAL(18, 245, 12), // "totalBytesIn"
QT_MOC_LITERAL(19, 258, 13), // "totalBytesOut"
QT_MOC_LITERAL(20, 272, 7), // "message"
QT_MOC_LITERAL(21, 280, 5), // "title"
QT_MOC_LITERAL(22, 286, 5), // "style"
QT_MOC_LITERAL(23, 292, 12), // "showProgress"
QT_MOC_LITERAL(24, 305, 9), // "nProgress"
QT_MOC_LITERAL(25, 315, 20), // "updateNumConnections"
QT_MOC_LITERAL(26, 336, 14), // "numConnections"
QT_MOC_LITERAL(27, 351, 19), // "updateNetworkActive"
QT_MOC_LITERAL(28, 371, 11), // "updateAlert"
QT_MOC_LITERAL(29, 383, 13) // "updateBanlist"

    },
    "ClientModel\0numConnectionsChanged\0\0"
    "count\0numBlocksChanged\0blockDate\0"
    "nVerificationProgress\0header\0"
    "SynchronizationState\0sync_state\0"
    "mempoolSizeChanged\0size_t\0mempoolSizeInBytes\0"
    "networkActiveChanged\0networkActive\0"
    "alertsChanged\0warnings\0bytesChanged\0"
    "totalBytesIn\0totalBytesOut\0message\0"
    "title\0style\0showProgress\0nProgress\0"
    "updateNumConnections\0numConnections\0"
    "updateNetworkActive\0updateAlert\0"
    "updateBanlist"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ClientModel[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      12,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       8,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   74,    2, 0x06 /* Public */,
       4,    5,   77,    2, 0x06 /* Public */,
      10,    2,   88,    2, 0x06 /* Public */,
      13,    1,   93,    2, 0x06 /* Public */,
      15,    1,   96,    2, 0x06 /* Public */,
      17,    2,   99,    2, 0x06 /* Public */,
      20,    3,  104,    2, 0x06 /* Public */,
      23,    2,  111,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      25,    1,  116,    2, 0x0a /* Public */,
      27,    1,  119,    2, 0x0a /* Public */,
      28,    0,  122,    2, 0x0a /* Public */,
      29,    0,  123,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int,    3,
    QMetaType::Void, QMetaType::Int, QMetaType::QDateTime, QMetaType::Double, QMetaType::Bool, 0x80000000 | 8,    3,    5,    6,    7,    9,
    QMetaType::Void, QMetaType::Long, 0x80000000 | 11,    3,   12,
    QMetaType::Void, QMetaType::Bool,   14,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::ULongLong, QMetaType::ULongLong,   18,   19,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::UInt,   21,   20,   22,
    QMetaType::Void, QMetaType::QString, QMetaType::Int,   21,   24,

 // slots: parameters
    QMetaType::Void, QMetaType::Int,   26,
    QMetaType::Void, QMetaType::Bool,   14,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void ClientModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ClientModel *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->numConnectionsChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 1: _t->numBlocksChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< const QDateTime(*)>(_a[2])),(*reinterpret_cast< double(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4])),(*reinterpret_cast< SynchronizationState(*)>(_a[5]))); break;
        case 2: _t->mempoolSizeChanged((*reinterpret_cast< long(*)>(_a[1])),(*reinterpret_cast< size_t(*)>(_a[2]))); break;
        case 3: _t->networkActiveChanged((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 4: _t->alertsChanged((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->bytesChanged((*reinterpret_cast< quint64(*)>(_a[1])),(*reinterpret_cast< quint64(*)>(_a[2]))); break;
        case 6: _t->message((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< uint(*)>(_a[3]))); break;
        case 7: _t->showProgress((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 8: _t->updateNumConnections((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 9: _t->updateNetworkActive((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 10: _t->updateAlert(); break;
        case 11: _t->updateBanlist(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ClientModel::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::numConnectionsChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(int , const QDateTime & , double , bool , SynchronizationState );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::numBlocksChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(long , size_t );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::mempoolSizeChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::networkActiveChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::alertsChanged)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(quint64 , quint64 );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::bytesChanged)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(const QString & , const QString & , unsigned int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::message)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (ClientModel::*)(const QString & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ClientModel::showProgress)) {
                *result = 7;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ClientModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ClientModel.data,
    qt_meta_data_ClientModel,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ClientModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ClientModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ClientModel.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ClientModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 12;
    }
    return _id;
}

// SIGNAL 0
void ClientModel::numConnectionsChanged(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ClientModel::numBlocksChanged(int _t1, const QDateTime & _t2, double _t3, bool _t4, SynchronizationState _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void ClientModel::mempoolSizeChanged(long _t1, size_t _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void ClientModel::networkActiveChanged(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void ClientModel::alertsChanged(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void ClientModel::bytesChanged(quint64 _t1, quint64 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void ClientModel::message(const QString & _t1, const QString & _t2, unsigned int _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void ClientModel::showProgress(const QString & _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
