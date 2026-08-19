// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactiondescdialog.h>
#include <qt/forms/ui_transactiondescdialog.h>

#include <qt/guiutil.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/wallet.h>

#include <QDialogButtonBox>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>

TransactionDescDialog::TransactionDescDialog(const QModelIndex &idx, WalletModel *wallet_model, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::TransactionDescDialog),
    m_wallet_model(wallet_model)
{
    ui->setupUi(this);
    const QString hash_str = idx.data(TransactionTableModel::TxHashRole).toString();
    setWindowTitle(tr("Details for %1").arg(hash_str));
    QString desc = idx.data(TransactionTableModel::LongDescriptionRole).toString();
    ui->detailText->setHtml(desc);

    m_txid.SetHex(hash_str.toStdString());
    if (m_wallet_model && m_wallet_model->wallet().transactionCanBeAbandoned(m_txid)) {
        // Beside the status that just explained why the payment is stuck, which
        // is the only place the remedy is of any use.
        m_abandon_button = ui->buttonBox->addButton(tr("A&bandon transaction"), QDialogButtonBox::ActionRole);
        m_abandon_button->setToolTip(tr("Give up on this payment and unlock the funds it was using, "
                                        "so you can spend them again."));
        connect(m_abandon_button, &QPushButton::clicked, this, &TransactionDescDialog::abandon);
    }

    GUIUtil::handleCloseWindowShortcut(this);
}

void TransactionDescDialog::abandon()
{
    if (!m_wallet_model) return;

    // Abandoning is not undoable from here, and it is not always the right
    // answer: a refusal can be lifted, and a transaction merely waiting to be
    // relayed is perfectly good. Say so before doing it.
    const QMessageBox::StandardButton answer = QMessageBox::question(this,
        tr("Abandon transaction"),
        tr("Give up on this payment?\n\n"
           "The funds it was using become spendable again. The payment is marked "
           "abandoned and will not be sent — though if the network has already "
           "seen it, it could still be confirmed."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    if (!m_wallet_model->wallet().abandonTransaction(m_txid)) {
        QMessageBox::warning(this, tr("Abandon transaction"),
            tr("This transaction can no longer be abandoned — the network has taken it "
               "in the meantime."));
        if (m_abandon_button) m_abandon_button->setEnabled(false);
        return;
    }
    // Close rather than redraw: the description is rendered once at construction,
    // and the row behind this dialog is about to say "Abandoned" for itself.
    accept();
}

TransactionDescDialog::~TransactionDescDialog()
{
    delete ui;
}
