// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRANSACTIONDESCDIALOG_H
#define BITCOIN_QT_TRANSACTIONDESCDIALOG_H

#include <QDialog>

#include <uint256.h>

class WalletModel;

namespace Ui {
    class TransactionDescDialog;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
class QPushButton;
QT_END_NAMESPACE

/** Dialog showing transaction details. */
class TransactionDescDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TransactionDescDialog(const QModelIndex &idx, WalletModel *wallet_model, QWidget *parent = nullptr);
    ~TransactionDescDialog();

private Q_SLOTS:
    void abandon();

private:
    Ui::TransactionDescDialog *ui;
    WalletModel* const m_wallet_model;
    uint256 m_txid;
    /** Shown only when the wallet says this transaction can be abandoned, which
     *  is the same condition under which the node may be refusing it. Nobody
     *  goes looking for a context menu on a row that claims to be pending. */
    QPushButton* m_abandon_button{nullptr};
};

#endif // BITCOIN_QT_TRANSACTIONDESCDIALOG_H
