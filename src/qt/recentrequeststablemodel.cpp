// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/recentrequeststablemodel.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <clientversion.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <streams.h>
#include <util/string.h>

#include <utility>

#include <QLatin1Char>
#include <QLatin1String>

RecentRequestsTableModel::RecentRequestsTableModel(WalletModel *parent) :
    QAbstractTableModel(parent), walletModel(parent)
{
    // Load entries from wallet
    for (const std::string& request : parent->wallet().getAddressReceiveRequests()) {
        addNewRequest(request);
    }

    /* These columns must match the indices in the ColumnIndex enumeration */
    columns << tr("Date") << tr("Label") << tr("Address");

    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RecentRequestsTableModel::updateDisplayUnit);
}

RecentRequestsTableModel::~RecentRequestsTableModel()
{
    /* Intentionally left empty */
}

int RecentRequestsTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return list.length();
}

int RecentRequestsTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return columns.length();
}

QVariant RecentRequestsTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid() || index.row() >= list.length())
        return QVariant();

    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        const RecentRequestEntry *rec = &list[index.row()];
        switch(index.column())
        {
        case Date:
            return GUIUtil::dateTimeStr(rec->date);
        case Label:
            if(rec->recipient.label.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no label)");
            }
            else
            {
                return rec->recipient.label;
            }
        case Address:
            return rec->recipient.address;
        }
    }
    return QVariant();
}

bool RecentRequestsTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || index.column() != Label || index.row() >= list.length())
        return false;
    RecentRequestEntry& rec = list[index.row()];
    const QString label = value.toString().trimmed();
    if (rec.recipient.label == label) return true;
    rec.recipient.label = label;
    // The label lives in two places: the stored request (this table) and the
    // wallet address book (what the transaction list and dialogs show). Keep
    // them in step, or an edit here would seem to have no effect elsewhere.
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << rec;
    const CTxDestination dest = DecodeDestination(rec.recipient.address.toStdString());
    walletModel->wallet().setAddressReceiveRequest(dest, ToString(rec.id), ss.str());
    walletModel->wallet().setAddressBook(dest, label.toStdString(), "receive");
    Q_EMIT dataChanged(index, index);
    return true;
}

QVariant RecentRequestsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
        {
            return columns[section];
        }
    }
    return QVariant();
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void RecentRequestsTableModel::updateAmountColumnTitle()
{
}

/** Gets title for amount column including current display unit if optionsModel reference available. */
QString RecentRequestsTableModel::getAmountTitle()
{
    if (!walletModel->getOptionsModel()) return {};
    return tr("Requested") +
           QLatin1String(" (") +
           BitcoinUnits::shortName(this->walletModel->getOptionsModel()->getDisplayUnit()) +
           QLatin1Char(')');
}

QModelIndex RecentRequestsTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return createIndex(row, column);
}

bool RecentRequestsTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
    Q_UNUSED(parent);

    if(count > 0 && row >= 0 && (row+count) <= list.size())
    {
        for (int i = 0; i < count; ++i)
        {
            const RecentRequestEntry* rec = &list[row+i];
            if (!walletModel->wallet().setAddressReceiveRequest(DecodeDestination(rec->recipient.address.toStdString()), ToString(rec->id), ""))
                return false;
        }

        beginRemoveRows(parent, row, row + count - 1);
        list.erase(list.begin() + row, list.begin() + row + count);
        endRemoveRows();
        return true;
    } else {
        return false;
    }
}

Qt::ItemFlags RecentRequestsTableModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // The label is the one user-owned field here: editable in place.
    if (index.column() == Label) f |= Qt::ItemIsEditable;
    return f;
}

// called when adding a request from the GUI
void RecentRequestsTableModel::addNewRequest(const SendCoinsRecipient &recipient)
{
    RecentRequestEntry newEntry;
    newEntry.id = ++nReceiveRequestsMaxId;
    newEntry.date = QDateTime::currentDateTime();
    newEntry.recipient = recipient;

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << newEntry;

    if (!walletModel->wallet().setAddressReceiveRequest(DecodeDestination(recipient.address.toStdString()), ToString(newEntry.id), ss.str()))
        return;

    addNewRequest(newEntry);
}

// called from ctor when loading from wallet
void RecentRequestsTableModel::addNewRequest(const std::string &recipient)
{
    std::vector<uint8_t> data(recipient.begin(), recipient.end());
    CDataStream ss(data, SER_DISK, CLIENT_VERSION);

    RecentRequestEntry entry;
    ss >> entry;

    if (entry.id == 0) // should not happen
        return;

    if (entry.id > nReceiveRequestsMaxId)
        nReceiveRequestsMaxId = entry.id;

    addNewRequest(entry);
}

// actually add to table in GUI
void RecentRequestsTableModel::addNewRequest(RecentRequestEntry &recipient)
{
    beginInsertRows(QModelIndex(), 0, 0);
    list.prepend(recipient);
    endInsertRows();
}

void RecentRequestsTableModel::sort(int column, Qt::SortOrder order)
{
    std::sort(list.begin(), list.end(), RecentRequestEntryLessThan(column, order));
    Q_EMIT dataChanged(index(0, 0, QModelIndex()), index(list.size() - 1, NUMBER_OF_COLUMNS - 1, QModelIndex()));
}

void RecentRequestsTableModel::updateDisplayUnit()
{
    updateAmountColumnTitle();
}

bool RecentRequestEntryLessThan::operator()(const RecentRequestEntry& left, const RecentRequestEntry& right) const
{
    const RecentRequestEntry* pLeft = &left;
    const RecentRequestEntry* pRight = &right;
    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch(column)
    {
    case RecentRequestsTableModel::Date:
        return pLeft->date.toSecsSinceEpoch() < pRight->date.toSecsSinceEpoch();
    case RecentRequestsTableModel::Label:
        return pLeft->recipient.label < pRight->recipient.label;
    case RecentRequestsTableModel::Address:
        return pLeft->recipient.address < pRight->recipient.address;
    default:
        return pLeft->id < pRight->id;
    }
}
