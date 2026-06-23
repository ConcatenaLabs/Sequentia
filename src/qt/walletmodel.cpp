// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/walletmodel.h>

#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>

#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/ui_interface.h>
#include <policy/policy.h>
#include <assetsdir.h>              // gAssetsDir, GetAssetFromString
#include <primitives/transaction.h> // g_con_any_asset_fees, CTransaction::GetFeeAsset
#include <psbt.h>
#include <util/system.h> // for GetBoolArg
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h> // for CRecipient
#include <rpc/util.h>

#include <stdint.h>
#include <functional>

#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>

using wallet::CCoinControl;
using wallet::CRecipient;
using wallet::DEFAULT_DISABLE_WALLET;

SendAssetsRecipient::SendAssetsRecipient(SendCoinsRecipient r) :
    address(r.address),
    label(r.label),
    asset(Params().GetConsensus().pegged_asset),
    asset_amount(r.amount),
    message(r.message),
    sPaymentRequest(r.sPaymentRequest),
    authenticatedMerchant(r.authenticatedMerchant),
    fSubtractFeeFromAmount(r.fSubtractFeeFromAmount)
{
}

#define SendCoinsRecipient SendAssetsRecipient

WalletModel::WalletModel(std::unique_ptr<interfaces::Wallet> wallet, ClientModel& client_model, const PlatformStyle *platformStyle, QObject *parent) :
    QObject(parent),
    m_wallet(std::move(wallet)),
    m_client_model(&client_model),
    m_node(client_model.node()),
    optionsModel(client_model.getOptionsModel()),
    addressTableModel(nullptr),
    transactionTableModel(nullptr),
    recentRequestsTableModel(nullptr),
    cachedEncryptionStatus(Unencrypted),
    timer(new QTimer(this))
{
    fHaveWatchOnly = m_wallet->haveWatchOnly();
    addressTableModel = new AddressTableModel(this);
    transactionTableModel = new TransactionTableModel(platformStyle, this);
    recentRequestsTableModel = new RecentRequestsTableModel(this);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

std::set<CAsset> WalletModel::getAssetTypes() const
{
    return cached_asset_types;
}

void WalletModel::startPollBalance()
{
    // This timer will be fired repeatedly to update the balance
    // Since the QTimer::timeout is a private signal, it cannot be used
    // in the GUIUtil::ExceptionSafeConnect directly.
    connect(timer, &QTimer::timeout, this, &WalletModel::timerTimeout);
    GUIUtil::ExceptionSafeConnect(this, &WalletModel::timerTimeout, this, &WalletModel::pollBalanceChanged);
    timer->start(MODEL_UPDATE_DELAY);
}

void WalletModel::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) timer->stop();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus) {
        Q_EMIT encryptionStatusChanged();
    }
}

void WalletModel::pollBalanceChanged()
{
    // Avoid recomputing wallet balances unless a TransactionChanged or
    // BlockTip notification was received.
    if (!fForceCheckBalanceChanged && m_cached_last_update_tip == getLastBlockProcessed()) return;

    // Try to get balances and return early if locks can't be acquired. This
    // avoids the GUI from getting stuck on periodical polls if the core is
    // holding the locks for a longer time - for example, during a wallet
    // rescan.
    interfaces::WalletBalances new_balances;
    uint256 block_hash;
    if (!m_wallet->tryGetBalances(new_balances, block_hash)) {
        return;
    }

    if (fForceCheckBalanceChanged || block_hash != m_cached_last_update_tip) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        m_cached_last_update_tip = block_hash;

        checkBalanceChanged(new_balances);
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();
    }
}

void WalletModel::checkBalanceChanged(const interfaces::WalletBalances& new_balances)
{
    if(new_balances.balanceChanged(m_cached_balances)) {
        m_cached_balances = new_balances;
        Q_EMIT balanceChanged(new_balances);

        std::set<CAsset> new_asset_types;
        for (const auto& assetamount : new_balances.balance + new_balances.unconfirmed_balance) {
            if (!assetamount.second) continue;
            new_asset_types.insert(assetamount.first);
        }
        if (new_asset_types != cached_asset_types) {
            cached_asset_types = new_asset_types;
            Q_EMIT assetTypesChanged();
        }
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, const QString &purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString &address)
{
    return IsValidDestinationString(address.toStdString());
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, wallet::BlindDetails *blind_details, const CCoinControl& coinControl)
{
    CAmountMap total;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    for (const SendCoinsRecipient &rcp : recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;
        {   // User-entered bitcoin address / amount:
            if(!validateAddress(rcp.address))
            {
                return InvalidAddress;
            }
            if(rcp.asset_amount <= 0)
            {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CTxDestination dest = DecodeDestination(rcp.address.toStdString());
            CScript scriptPubKey = GetScriptForDestination(dest);
            CPubKey confidentiality_pubkey = GetDestinationBlindingKey(dest);
            CRecipient recipient = {scriptPubKey, rcp.asset_amount, rcp.asset, confidentiality_pubkey, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);

            total[rcp.asset] += rcp.asset_amount;
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    CAmountMap nBalance = m_wallet->getAvailableBalance(coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    {
        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        bilingual_str error;

        auto& newTx = transaction.getWtx();
        std::vector<CAmount> out_amounts;
        newTx = m_wallet->createTransaction(vecSend, coinControl, !wallet().privateKeysDisabled() /* sign */, nChangePosRet, nFeeRequired, blind_details, error);
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && newTx) {
            if(blind_details) {
                out_amounts = blind_details->o_amounts;
                assert(out_amounts.size() == newTx->vout.size());
            }
            transaction.reassignAmounts(out_amounts, nChangePosRet);
        }

        if(!newTx)
        {
            total[Params().GetConsensus().pegged_asset] += nFeeRequired;
            if(!fSubtractFeeFromAmount && total > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(error.translated),
                CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // Reject absurdly high fee. (This can never happen because the
        // wallet never creates transactions with fee greater than
        // m_default_max_tx_fee. This merely a belt-and-suspenders check).
        if (nFeeRequired > m_wallet->getDefaultMaxTxFee()) {
            return AbsurdFee;
        }
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction &transaction, wallet::BlindDetails *blind_details)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        std::vector<std::pair<std::string, std::string>> vOrderForm;
        for (const SendCoinsRecipient &rcp : transaction.getRecipients())
        {
            if (!rcp.message.isEmpty()) // Message from normal bitcoin:URI (bitcoin:123...?message=example)
                vOrderForm.emplace_back("Message", rcp.message.toStdString());
        }

        auto& newTx = transaction.getWtx();
        wallet().commitTransaction(newTx, {} /* mapValue */, std::move(vOrderForm), blind_details);

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append((const char*)ssTx.data(), ssTx.size());
    }

    // Add addresses / update labels that we've sent to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendCoinsRecipient &rcp : transaction.getRecipients())
    {
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                // Check if we have a new address or an updated label
                std::string name;
                if (!m_wallet->getAddress(
                     dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr))
                {
                    m_wallet->setAddressBook(dest, strLabel, "send");
                }
                else if (name != strLabel)
                {
                    m_wallet->setAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(this, rcp, transaction_array);
    }

    checkBalanceChanged(m_wallet->getBalances()); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

RecentRequestsTableModel *WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!m_wallet->isCrypted())
    {
        // A previous bug allowed for watchonly wallets to be encrypted (encryption keys set, but nothing is actually encrypted).
        // To avoid misrepresenting the encryption status of such wallets, we only return NoKeys for watchonly wallets that are unencrypted.
        if (m_wallet->privateKeysDisabled()) {
            return NoKeys;
        }
        return Unencrypted;
    }
    else if(m_wallet->isLocked())
    {
        return Locked;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(const SecureString& passphrase)
{
    return m_wallet->encryptWallet(passphrase);
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    if(locked)
    {
        // Lock
        return m_wallet->lock();
    }
    else
    {
        // Unlock
        return m_wallet->unlock(passPhrase);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    m_wallet->lock(); // Make sure wallet is locked before attempting pass change
    return m_wallet->changeWalletPassphrase(oldPass, newPass);
}

// Handlers for core signals
static void NotifyUnload(WalletModel* walletModel)
{
    qDebug() << "NotifyUnload";
    bool invoked = QMetaObject::invokeMethod(walletModel, "unload");
    assert(invoked);
}

static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel,
        const CTxDestination &address, const std::string &label, bool isMine,
        const std::string &purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(EncodeDestination(address));
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(QString, strPurpose),
                              Q_ARG(int, status));
    assert(invoked);
}

static void NotifyTransactionChanged(WalletModel *walletmodel, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(hash);
    Q_UNUSED(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
    assert(invoked);
}

static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    bool invoked = QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
    assert(invoked);
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
    assert(invoked);
}

static void NotifyCanGetAddressesChanged(WalletModel* walletmodel)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "canGetAddressesChanged");
    assert(invoked);
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_unload = m_wallet->handleUnload(std::bind(&NotifyUnload, this));
    m_handler_status_changed = m_wallet->handleStatusChanged(std::bind(&NotifyKeyStoreStatusChanged, this));
    m_handler_address_book_changed = m_wallet->handleAddressBookChanged(std::bind(NotifyAddressBookChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    m_handler_transaction_changed = m_wallet->handleTransactionChanged(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_show_progress = m_wallet->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_watch_only_changed = m_wallet->handleWatchOnlyChanged(std::bind(NotifyWatchonlyChanged, this, std::placeholders::_1));
    m_handler_can_get_addrs_changed = m_wallet->handleCanGetAddressesChanged(std::bind(NotifyCanGetAddressesChanged, this));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_unload->disconnect();
    m_handler_status_changed->disconnect();
    m_handler_address_book_changed->disconnect();
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_watch_only_changed->disconnect();
    m_handler_can_get_addrs_changed->disconnect();
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, bool _relock):
        wallet(_wallet),
        valid(_valid),
        relock(_relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

void WalletModel::UnlockContext::CopyFrom(UnlockContext&& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.relock = false;
}

bool WalletModel::bumpFee(uint256 hash, uint256& new_hash)
{
    CCoinControl coin_control;
    coin_control.m_signal_bip125_rbf = true;

    // Recover the ORIGINAL tx's fee asset — the default for the bump and for display.
    CAsset old_fee_asset = ::policyAsset;
    if (CTransactionRef orig = m_wallet->getTx(hash)) {
        old_fee_asset = orig->GetFeeAsset(::policyAsset);
    }
    CAsset new_fee_asset = old_fee_asset; // unless the user switches it below

    // Sequentia any-asset fees: let the user choose which asset pays the increased fee — any
    // held asset; no asset is privileged. The default keeps the original tx's fee asset (also
    // what feebumper pins when m_fee_asset is unset). Switching to a more widely accepted asset
    // is how a stranded any-asset-fee tx is rescued.
    if (g_con_any_asset_fees) {
        QStringList labels; QList<QString> hexes;
        labels << tr("Keep original (%1)").arg(QString::fromStdString(gAssetsDir.GetIdentifier(old_fee_asset)));
        hexes  << QString::fromStdString(old_fee_asset.GetHex());
        for (const CAsset& asset : getAssetTypes()) {
            if (asset == old_fee_asset) continue;
            labels << QString::fromStdString(gAssetsDir.GetIdentifier(asset));
            hexes  << QString::fromStdString(asset.GetHex());
        }
        if (labels.size() > 1) {
            QDialog dlg(nullptr);
            dlg.setWindowTitle(tr("Choose fee asset"));
            auto* lay = new QVBoxLayout(&dlg);
            lay->addWidget(new QLabel(tr("Pay the increased fee in:"), &dlg));
            auto* combo = new QComboBox(&dlg);
            for (int i = 0; i < labels.size(); ++i) combo->addItem(labels[i], hexes[i]);
            lay->addWidget(combo);
            auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            lay->addWidget(bb);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) return false;
            const CAsset sel = GetAssetFromString(combo->currentData().toString().toStdString());
            if (!sel.IsNull() && sel != old_fee_asset) {
                coin_control.m_fee_asset = sel; // honored by feebumper
                new_fee_asset = sel;
            }
        }
    }

    std::vector<bilingual_str> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    if (!m_wallet->createBumpTransaction(hash, coin_control, errors, old_fee, new_fee, mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Increasing transaction fee failed") + "<br />(" +
            (errors.size() ? QString::fromStdString(errors[0].translated) : "") +")");
        return false;
    }

    // allow a user based fee verification
    /*: Asks a user if they would like to manually increase the fee of a transaction that has already been created. */
    QString questionString = tr("Do you want to increase the fee?");
    questionString.append("<br />");
    const int unit = getOptionsModel()->getDisplayUnit();
    questionString.append("<table style=\"text-align: left;\">");
    questionString.append("<tr><td>");
    questionString.append(tr("Current fee:"));
    questionString.append("</td><td>");
    questionString.append(GUIUtil::formatAssetAmount(old_fee_asset, old_fee, unit, BitcoinUnits::SeparatorStyle::STANDARD, true));
    questionString.append("</td></tr>");
    if (new_fee_asset == old_fee_asset) {
        questionString.append("<tr><td>");
        questionString.append(tr("Increase:"));
        questionString.append("</td><td>");
        questionString.append(GUIUtil::formatAssetAmount(new_fee_asset, new_fee - old_fee, unit, BitcoinUnits::SeparatorStyle::STANDARD, true));
        questionString.append("</td></tr>");
    }
    // A cross-asset switch makes the new-minus-old diff meaningless, so omit the Increase row.
    questionString.append("<tr><td>");
    questionString.append(tr("New fee:"));
    questionString.append("</td><td>");
    questionString.append(GUIUtil::formatAssetAmount(new_fee_asset, new_fee, unit, BitcoinUnits::SeparatorStyle::STANDARD, true));
    questionString.append("</td></tr></table>");

    // Display warning in the "Confirm fee bump" window if the "Coin Control Features" option is enabled
    if (getOptionsModel()->getCoinControlFeatures()) {
        questionString.append("<br><br>");
        questionString.append(tr("Warning: This may pay the additional fee by reducing change outputs or adding inputs, when necessary. It may add a new change output if one does not already exist. These changes may potentially leak privacy."));
    }

    auto confirmationDialog = new SendConfirmationDialog(tr("Confirm fee bump"), questionString, "", "", SEND_CONFIRM_DELAY, !m_wallet->privateKeysDisabled(), getOptionsModel()->getEnablePSBTControls(), nullptr);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    // cancel sign&broadcast if user doesn't want to bump the fee
    if (retval != QMessageBox::Yes && retval != QMessageBox::Save) {
        return false;
    }

    WalletModel::UnlockContext ctx(requestUnlock());
    if(!ctx.isValid())
    {
        return false;
    }

    // Short-circuit if we are returning a bumped transaction PSBT to clipboard
    if (retval == QMessageBox::Save) {
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const TransactionError err = wallet().fillPSBT(SIGHASH_ALL, false /* sign */, true /* bip32derivs */, nullptr, psbtx, complete);
        if (err != TransactionError::OK || complete) {
            QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't draft transaction."));
            return false;
        }
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
        Q_EMIT message(tr("PSBT copied"), "Copied to clipboard", CClientUIInterface::MSG_INFORMATION);
        return true;
    }

    assert(!m_wallet->privateKeysDisabled());

    // sign bumped transaction
    if (!m_wallet->signBumpTransaction(mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't sign transaction."));
        return false;
    }
    // commit the bumped transaction
    if(!m_wallet->commitBumpTransaction(hash, std::move(mtx), errors, new_hash)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Could not commit transaction") + "<br />(" +
            QString::fromStdString(errors[0].translated)+")");
        return false;
    }
    return true;
}

bool WalletModel::canDoCPFP(uint256 hash)
{
    interfaces::WalletTxStatus st;
    interfaces::WalletOrderForm of;
    bool in_mempool = false;
    int nblocks = 0;
    interfaces::WalletTx wtx = m_wallet->getWalletTxDetails(hash, st, of, in_mempool, nblocks);
    if (!wtx.tx) return false;
    if (st.depth_in_main_chain != 0 || !in_mempool || st.is_abandoned) return false;
    for (size_t n = 0; n < wtx.tx->vout.size(); ++n) {
        if (n >= wtx.txout_is_mine.size() || wtx.txout_is_mine[n] == wallet::ISMINE_NO) continue;
        const auto coins = m_wallet->getCoins({COutPoint(hash, (uint32_t)n)});
        if (coins.empty() || coins[0].is_spent) continue;
        return true; // an unconfirmed, unspent, wallet-owned output to attach a child to
    }
    return false;
}

bool WalletModel::createChildPaysForParent(uint256 parentHash, uint256& childHash)
{
    interfaces::WalletTxStatus st;
    interfaces::WalletOrderForm of;
    bool in_mempool = false;
    int nblocks = 0;
    interfaces::WalletTx wtx = m_wallet->getWalletTxDetails(parentHash, st, of, in_mempool, nblocks);
    if (!wtx.tx || st.depth_in_main_chain != 0 || !in_mempool) {
        QMessageBox::critical(nullptr, tr("Speed up"), tr("This transaction is no longer unconfirmed."));
        return false;
    }

    // Pick a spendable wallet-owned output of the parent — prefer its change.
    std::optional<size_t> pick;
    for (size_t n = 0; n < wtx.tx->vout.size(); ++n) {
        if (n >= wtx.txout_is_mine.size() || wtx.txout_is_mine[n] == wallet::ISMINE_NO) continue;
        const auto coins = m_wallet->getCoins({COutPoint(parentHash, (uint32_t)n)});
        if (coins.empty() || coins[0].is_spent) continue;
        if (n < wtx.txout_is_change.size() && wtx.txout_is_change[n]) { pick = n; break; }
        if (!pick) pick = n;
    }
    if (!pick) {
        QMessageBox::critical(nullptr, tr("Speed up"), tr("No spendable output to attach a child fee to."));
        return false;
    }
    const size_t n = *pick;
    const CAsset childAsset = wtx.txout_assets[n];
    const CAmount childValue = wtx.txout_amounts[n];

    // CPFP: spend the parent's unconfirmed output (the link a miner evaluates as a package) and pay
    // a HIGH child fee, funded from the wallet in a PRODUCER-ACCEPTED asset (default the policy
    // asset, which producers always mine) — NOT confined to the pinned output's asset, which may be
    // exactly the one producers reject (confining the fee to the change asset was the old bug, and
    // makes the child strand alongside the parent). The user may pick any accepted asset.
    CAsset feeAsset = ::policyAsset;
    if (g_con_any_asset_fees) {
        QStringList labels; QList<QString> hexes;
        labels << tr("tSEQ (recommended)");
        hexes  << QString::fromStdString(::policyAsset.GetHex());
        for (const CAsset& asset : getAssetTypes()) {
            if (asset == ::policyAsset) continue;
            labels << QString::fromStdString(gAssetsDir.GetIdentifier(asset));
            hexes  << QString::fromStdString(asset.GetHex());
        }
        if (labels.size() > 1) {
            QDialog dlg(nullptr);
            dlg.setWindowTitle(tr("Speed up — fee asset"));
            auto* lay = new QVBoxLayout(&dlg);
            lay->addWidget(new QLabel(tr("Pay the child fee in (a producer-accepted asset lets the package confirm):"), &dlg));
            auto* combo = new QComboBox(&dlg);
            for (int i = 0; i < labels.size(); ++i) combo->addItem(labels[i], hexes[i]);
            lay->addWidget(combo);
            auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            lay->addWidget(bb);
            connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) return false;
            feeAsset = GetAssetFromString(combo->currentData().toString().toStdString());
            if (feeAsset.IsNull()) feeAsset = ::policyAsset;
        }
    }

    CCoinControl cc;
    cc.Select(COutPoint(parentHash, (uint32_t)n));
    cc.fAllowOtherInputs = true;       // pull in fee-asset funds to pay the child fee
    cc.m_include_unsafe_inputs = true; // an unconfirmed output we own is not "trusted"
    cc.m_signal_bip125_rbf = true;
    if (g_con_any_asset_fees && feeAsset != ::policyAsset) cc.m_fee_asset = feeAsset;

    // Package-aware feerate: size the child so that, even crediting the parent with zero effective
    // fee, the {parent, child} package clears the target (mirrors Wollet::cpfp_suggested_feerate).
    const CAmount min_per_k = m_wallet->getMinimumFee(1000, cc, nullptr, nullptr);
    const int64_t parent_vsize = GetVirtualTransactionSize(*wtx.tx);
    const int64_t child_vsize = 1100; // conservative confidential-child estimate
    const CAmount target_per_k = min_per_k * 5; // a healthy confirmation target
    cc.m_feerate = CFeeRate(target_per_k * (parent_vsize + child_vsize) / child_vsize);
    cc.fOverrideFeeRate = true;

    // Send the pinned output's value back to the wallet in its OWN asset. Subtract the fee from it
    // only when the fee is paid in that same asset; otherwise preserve it and fund the fee from the
    // separately-selected fee-asset inputs.
    const bool sameAsset = (feeAsset == childAsset);
    CTxDestination dest;
    if (!m_wallet->getNewDestination(m_wallet->getDefaultAddressType(), "CPFP", dest, /*add_blinding_key=*/true)) {
        QMessageBox::critical(nullptr, tr("Speed up"), tr("Could not get a new address."));
        return false;
    }
    CScript spk = GetScriptForDestination(dest);
    CPubKey blind = GetDestinationBlindingKey(dest);
    std::vector<CRecipient> vecSend{ {spk, childValue, childAsset, blind, /*fSubtractFeeFromAmount=*/sameAsset} };

    UnlockContext ctx(requestUnlock());
    if (!ctx.isValid()) return false;

    // Elements txs require BlindDetails (asserted in elements mode); createTransaction fills it.
    auto blind_details = std::make_unique<wallet::BlindDetails>();
    int changePos = -1;
    CAmount fee = 0;
    bilingual_str err;
    CTransactionRef child = m_wallet->createTransaction(
        vecSend, cc, !wallet().privateKeysDisabled() /*sign*/, changePos, fee, blind_details.get(), err);
    if (!child) {
        QMessageBox::critical(nullptr, tr("Speed up"), tr("Could not create the child transaction") + "<br />(" +
            QString::fromStdString(err.translated) + ")");
        return false;
    }
    m_wallet->commitTransaction(child, {} /* mapValue */, {} /* orderForm */, blind_details.get());
    childHash = child->GetHash();
    return true;
}

bool WalletModel::displayAddress(std::string sAddress)
{
    CTxDestination dest = DecodeDestination(sAddress);
    bool res = false;
    try {
        res = m_wallet->displayAddress(dest);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Can't display address"), e.what());
    }
    return res;
}

bool WalletModel::isWalletEnabled()
{
   return !gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
}

QString WalletModel::getWalletName() const
{
    return QString::fromStdString(m_wallet->getWalletName());
}

QString WalletModel::getDisplayName() const
{
    const QString name = getWalletName();
    return name.isEmpty() ? "["+tr("default wallet")+"]" : name;
}

bool WalletModel::isMultiwallet()
{
    return m_node.walletLoader().getWallets().size() > 1;
}

void WalletModel::refresh(bool pk_hash_only)
{
    addressTableModel = new AddressTableModel(this, pk_hash_only);
}

uint256 WalletModel::getLastBlockProcessed() const
{
    return m_client_model ? m_client_model->getBestBlockHash() : uint256{};
}
