// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/sendcoinsdialog.h>
#include <qt/forms/ui_sendcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendcoinsentry.h>

#include <chainparams.h>
#include <exchangerates.h>
#include <feeassets.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/ui_interface.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <assetsdir.h>
#include <primitives/transaction.h>
#include <txmempool.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>

#include <QFontMetrics>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QTextDocument>

using wallet::CCoinControl;
using wallet::DEFAULT_PAY_TX_FEE;

static constexpr std::array confTargets{2, 4, 6, 12, 24, 48, 144, 504, 1008};
int getConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(confTargets.size())) {
        return confTargets.back();
    }
    if (index < 0) {
        return confTargets[0];
    }
    return confTargets[index];
}
int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < confTargets.size(); i++) {
        if (confTargets[i] >= target) {
            return i;
        }
    }
    return confTargets.size() - 1;
}

SendCoinsDialog::SendCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::SendCoinsDialog),
    clientModel(nullptr),
    model(nullptr),
    m_coin_control(new CCoinControl),
    fNewRecipientAllowed(true),
    fFeeMinimized(true),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->addButton->setIcon(QIcon());
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->addButton->setIcon(_platformStyle->SingleColorIcon(":/icons/add"));
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->sendButton->setIcon(_platformStyle->SingleColorIcon(":/icons/send"));
    }

    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    addEntry();

    connect(ui->addButton, &QPushButton::clicked, this, &SendCoinsDialog::addEntry);
    connect(ui->clearButton, &QPushButton::clicked, this, &SendCoinsDialog::clear);

    // Coin Control
    connect(ui->pushButtonCoinControl, &QPushButton::clicked, this, &SendCoinsDialog::coinControlButtonClicked);
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged, this, &SendCoinsDialog::coinControlChangeChecked);
    connect(ui->lineEditCoinControlChange, &QValidatedLineEdit::textEdited, this, &SendCoinsDialog::coinControlChangeEdited);

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardBytes);
    connect(clipboardLowOutputAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardLowOutput);
    connect(clipboardChangeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardChange);
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    // init transaction fee section
    QSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nTransactionFee"))
        settings.setValue("nTransactionFee", (qint64)DEFAULT_PAY_TX_FEE);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->customFee->SetAllowEmpty(false);
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    GUIUtil::ExceptionSafeConnect(ui->sendButton, &QPushButton::clicked, this, &SendCoinsDialog::sendButtonClicked);
}

void SendCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &SendCoinsDialog::updateNumberOfBlocks);
    }
}

void SendCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        interfaces::WalletBalances balances = _model->wallet().getBalances();
        setBalance(balances);
        connect(_model, &WalletModel::balanceChanged, this, &SendCoinsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::updateDisplayUnit);
        connect(_model->getOptionsModel(), &OptionsModel::referenceCurrencyChanged, this, &SendCoinsDialog::updateDisplayUnit);
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &SendCoinsDialog::coinControlFeatureChanged);
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        // fee section
        for (const int n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n*Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::updateSmartFeeLabel);
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::coinControlUpdateLabels);

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::coinControlUpdateLabels);
#else
        connect(ui->groupFee, qOverload<int>(&QButtonGroup::buttonClicked), this, &SendCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, qOverload<int>(&QButtonGroup::buttonClicked), this, &SendCoinsDialog::coinControlUpdateLabels);
#endif

        connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        connect(ui->optInRBF, &QCheckBox::stateChanged, this, &SendCoinsDialog::updateSmartFeeLabel);
        connect(ui->optInRBF, &QCheckBox::stateChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        // The unpriced-fee-asset warning tells the user to turn RBF on, so it has
        // to stop saying that the moment they do.
        connect(ui->optInRBF, &QCheckBox::stateChanged, this, [this](int) { updateFeeAssetWarning(); });
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->SetMinValue(requiredFee);
        if (ui->customFee->value() < requiredFee) {
            ui->customFee->setValue(requiredFee);
        }
        ui->customFee->setSingleStep(requiredFee);
        updateFeeSectionControls();
        updateSmartFeeLabel();

        // set default rbf checkbox state
        ui->optInRBF->setCheckState(Qt::Checked);

        // Sequentia any-asset fees: let the user pay the fee in any asset they hold. The fee
        // asset is a free choice — no asset is privileged. The default follows the asset being
        // sent while this node accepts a fee in it (see updateDefaultFeeAsset); a pick this
        // node will not value, or one no other producer is likely to, gets a warning.
        ui->feeAssetSelector->setVisible(g_con_any_asset_fees);
        ui->labelFeeAssetWarning->setVisible(false);
        if (g_con_any_asset_fees) {
            auto populateFeeAssets = [this]() {
                const QString prev = ui->feeAssetSelector->currentData().toString();
                ui->feeAssetSelector->clear();
                ui->feeAssetSelector->addItem(GUIUtil::assetDisplayName(::policyAsset),
                                              QString::fromStdString(::policyAsset.GetHex()));
                // Reissuance tokens are not on offer: the fee is paid to whichever
                // producer mines this transaction, into its coinbase, so a fee in an
                // inflation key would give that producer the power to mint the asset
                // without limit. Any fraction of the token carries the whole power,
                // which is why there is no "small enough" amount to spend on a fee.
                for (const CAsset& asset : model->getFeePayableAssetTypes()) {
                    if (asset == ::policyAsset) continue;
                    ui->feeAssetSelector->addItem(GUIUtil::assetDisplayName(asset),
                                                  QString::fromStdString(asset.GetHex()));
                }
                const int idx = ui->feeAssetSelector->findData(prev);
                if (idx >= 0) ui->feeAssetSelector->setCurrentIndex(idx);
                updateDefaultFeeAsset();
            };
            populateFeeAssets();
            connect(model, &WalletModel::assetTypesChanged, this, populateFeeAssets);
            connect(ui->feeAssetSelector, qOverload<int>(&QComboBox::currentIndexChanged),
                    this, &SendCoinsDialog::coinControlUpdateLabels);
            connect(ui->feeAssetSelector, qOverload<int>(&QComboBox::currentIndexChanged),
                    this, [this](int) { updateFeeAssetWarning(); });
            // The fee headline quotes the rate in the selected asset, so it has to
            // follow the selector, not just the confirmation target.
            connect(ui->feeAssetSelector, qOverload<int>(&QComboBox::currentIndexChanged),
                    this, &SendCoinsDialog::updateSmartFeeLabel);
            // activated() fires only on a real user pick (never programmatically): from
            // then on the user's choice is respected until the form is cleared.
            connect(ui->feeAssetSelector, qOverload<int>(&QComboBox::activated),
                    this, [this](int) { m_fee_asset_user_choice = true; });
        }

        if (model->wallet().hasExternalSigner()) {
            //: "device" usually means a hardware wallet.
            ui->sendButton->setText(tr("Sign on device"));
            if (gArgs.GetArg("-signer", "") != "") {
                ui->sendButton->setEnabled(true);
                ui->sendButton->setToolTip(tr("Connect your hardware wallet first."));
            } else {
                ui->sendButton->setEnabled(false);
                //: "External signer" means using devices such as hardware wallets.
                ui->sendButton->setToolTip(tr("Set external signer script path in Options -> Wallet"));
            }
        } else if (model->wallet().privateKeysDisabled()) {
            ui->sendButton->setText(tr("Cr&eate Unsigned"));
            ui->sendButton->setToolTip(tr("Creates a Partially Signed Bitcoin Transaction (PSBT) for use with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
        }

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        QSettings settings;
        if (settings.value("nSmartFeeSliderPosition").toInt() != 0) {
            // migrate nSmartFeeSliderPosition to nConfTarget
            // nConfTarget is available since 0.15 (replaced nSmartFeeSliderPosition)
            int nConfirmTarget = 25 - settings.value("nSmartFeeSliderPosition").toInt(); // 25 == old slider range
            settings.setValue("nConfTarget", nConfirmTarget);
            settings.remove("nSmartFeeSliderPosition");
        }
        if (settings.value("nConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(model->wallet().getConfirmTarget()));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    QSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());

    delete ui;
}

bool SendCoinsDialog::PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text)
{
    QList<SendAssetsRecipient> recipients;
    bool valid = true;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate(model->node()))
            {
                recipients.append(entry->getValue());
            }
            else if (valid)
            {
                ui->scrollArea->ensureWidgetVisible(entry);
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return false;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return false;
    }

    // prepare transaction for getting txFee earlier
    m_current_transaction = std::make_unique<WalletModelTransaction>(recipients);
    if (g_con_elementsmode)
        m_current_blind_details = std::make_unique<wallet::BlindDetails>();
    WalletModel::SendCoinsReturn prepareStatus;

    updateCoinControlState();

    prepareStatus = model->prepareTransaction(*m_current_transaction, m_current_blind_details.get(), *m_coin_control);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), m_current_transaction->getTransactionFee()));

    if(prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
        return false;
    }

    CAmount txFee = m_current_transaction->getTransactionFee();
    int bitcoin_unit = model->getOptionsModel()->getDisplayUnit();
    QStringList formatted;
    // SEQUENTIA: which of these recipients is being sent an inflation key, named by
    // the asset it mints. Warned about below, before the transaction is created.
    QStringList inflation_keys;
    for (const SendAssetsRecipient &rcp : m_current_transaction->getRecipients())
    {
        CAsset minted;
        if (GUIUtil::isReissuanceToken(rcp.asset, &minted)) {
            const QString name = GUIUtil::assetIsNamed(minted)
                ? GUIUtil::assetDisplayName(minted)
                : GUIUtil::ellipsizeMiddle(QString::fromStdString(minted.GetHex()));
            if (!inflation_keys.contains(name)) inflation_keys.append(name);
        }
        // generate amount string with wallet name in case of multiwallet
        QString amount = GUIUtil::formatAssetAmount(rcp.asset, rcp.asset_amount, bitcoin_unit, BitcoinUnits::SeparatorStyle::STANDARD, true);
        // SEQUENTIA: append the amount valued in the user's reference currency (display only).
        const QString amountRef = GUIUtil::formatReferenceApprox(rcp.asset, rcp.asset_amount, QString());
        if (!amountRef.isEmpty()) {
            amount.append(QString(" <span style='color:#888'>%1</span>").arg(amountRef.toHtmlEscaped()));
        }
        if (model->isMultiwallet()) {
            amount.append(tr(" from wallet '%1'").arg(GUIUtil::HtmlEscape(model->getWalletName())));
        }

        // generate address string
        QString address = rcp.address;

        QString recipientElement;

        {
            if(rcp.label.length() > 0) // label with address
            {
                recipientElement.append(tr("%1 to '%2'").arg(amount, GUIUtil::HtmlEscape(rcp.label)));
                recipientElement.append(QString(" (%1)").arg(address));
            }
            else // just address
            {
                recipientElement.append(tr("%1 to %2").arg(amount, address));
            }
        }
        formatted.append(recipientElement);
    }

    /*: Message displayed when attempting to create a transaction. Cautionary text to prompt the user to verify
        that the displayed transaction details represent the transaction the user intends to create. */
    question_string.append(tr("Do you want to create this transaction?"));
    question_string.append("<br /><span style='font-size:10pt;'>");
    if (model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can only create a PSBT. This string is displayed when private keys are disabled and an external
            signer is not available. */
        question_string.append(tr("Please, review your transaction proposal. This will produce a Partially Signed Bitcoin Transaction (PSBT) which you can save or copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
    } else if (model->getOptionsModel()->getEnablePSBTControls()) {
        /*: Text to inform a user attempting to create a transaction of their current options. At this stage,
            a user can send their transaction or create a PSBT. This string is displayed when both private keys
            and PSBT controls are enabled. */
        question_string.append(tr("Please, review your transaction. You can create and send this transaction or create a Partially Signed Bitcoin Transaction (PSBT), which you can save or copy and then sign with, e.g., an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
    } else {
        /*: Text to prompt a user to review the details of the transaction they are attempting to send. */
        question_string.append(tr("Please, review your transaction."));
    }
    question_string.append("</span>%1");

    // SEQUENTIA: an inflation key does not behave like an amount of anything. Whoever
    // holds any fraction of one can mint the asset without limit, and sending part of
    // it does not divide that power, it copies it -- the recipient gains the mint and
    // the sender does not lose it, so a "small" transfer is the full handover. Say so
    // here, where the transaction can still be abandoned, rather than leaving the
    // holder to discover it from someone else's issuance.
    if (!inflation_keys.isEmpty()) {
        question_string.append("<hr /><span style='color:#ff6b6b; font-weight:bold;'>");
        question_string.append(tr("You are sending an inflation key (%1).")
                                   .arg(inflation_keys.join(", ").toHtmlEscaped()));
        question_string.append("</span><br /><span style='font-size:10pt;'>");
        question_string.append(tr("Whoever receives it can mint that asset in any quantity, for ever. "
                                  "The power is copied, not divided: sending a fraction gives the "
                                  "recipient the whole mint while you keep yours, and there is no way "
                                  "to take it back afterwards."));
        question_string.append("</span>");
    }

    if(txFee > 0)
    {
        // append fee string if a fee is required
        question_string.append("<hr /><b>");
        question_string.append(tr("Transaction fee"));
        question_string.append("</b>");

        // append transaction size
        question_string.append(" (" + QString::number((double)m_current_transaction->getTransactionSize() / 1000) + " kB): ");

        // append transaction fee value
        question_string.append("<span style='color:#ff6b6b; font-weight:bold;'>");
        question_string.append(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        question_string.append("</span><br />");

        // append RBF message according to transaction's signalling
        question_string.append("<span style='font-size:10pt; font-weight:normal;'>");
        if (ui->optInRBF->isChecked()) {
            question_string.append(tr("You can increase the fee later (signals Replace-By-Fee, BIP-125)."));
        } else {
            question_string.append(tr("Not signalling Replace-By-Fee, BIP-125."));
        }
        question_string.append("</span>");
    }

    // add total amount in all subdivision units
    question_string.append("<hr />");
    CAmountMap totalAmount = m_current_transaction->getTotalTransactionAmount();
    totalAmount[Params().GetConsensus().pegged_asset] += txFee;
    QStringList alternativeUnits;
    for (const BitcoinUnits::Unit u : BitcoinUnits::availableUnits())
    {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount[Params().GetConsensus().pegged_asset]));
    }
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
        .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount[Params().GetConsensus().pegged_asset])));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeUnits.join(" " + tr("or") + " ")));
    totalAmount.erase(Params().GetConsensus().pegged_asset);
    if (!!totalAmount) {
        question_string.append(" " + tr("and") + "<br />" + GUIUtil::formatMultiAssetAmount(totalAmount, -1 /*bitcoin unit, hide*/, BitcoinUnits::SeparatorStyle::STANDARD, ";<br />"));
    }

    if (formatted.size() > 1) {
        question_string = question_string.arg("");
        informative_text = tr("To review recipient list click \"Show Details…\"");
        detailed_text = formatted.join("\n\n");
    } else {
        question_string = question_string.arg("<br /><br />" + formatted.at(0));
    }

    return true;
}

void SendCoinsDialog::presentPSBT(PartiallySignedTransaction& psbtx)
{
    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    QMessageBox msgBox;
    msgBox.setText("Unsigned Transaction");
    msgBox.setInformativeText("The PSBT has been copied to the clipboard. You can also save it.");
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    msgBox.setDefaultButton(QMessageBox::Discard);
    switch (msgBox.exec()) {
    case QMessageBox::Save: {
        int bitcoin_unit = model->getOptionsModel()->getDisplayUnit();
        QString selectedFilter;
        QString fileNameSuggestion = "";
        bool first = true;
        for (const SendAssetsRecipient &rcp : m_current_transaction->getRecipients()) {
            if (!first) {
                fileNameSuggestion.append(" - ");
            }
            QString labelOrAddress = rcp.label.isEmpty() ? rcp.address : rcp.label;
            QString amount = GUIUtil::formatAssetAmount(rcp.asset, rcp.asset_amount, bitcoin_unit, BitcoinUnits::SeparatorStyle::STANDARD, true);
            fileNameSuggestion.append(labelOrAddress + "-" + amount);
            first = false;
        }
        fileNameSuggestion.append(".psbt");
        QString filename = GUIUtil::getSaveFileName(this,
            tr("Save Transaction Data"), fileNameSuggestion,
            //: Expanded name of the binary PSBT file format. See: BIP 174.
            tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selectedFilter);
        if (filename.isEmpty()) {
            return;
        }
        std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
        out << ssTx.str();
        out.close();
        Q_EMIT message(tr("PSBT saved"), "PSBT saved to disk", CClientUIInterface::MSG_INFORMATION);
        break;
    }
    case QMessageBox::Discard:
        break;
    default:
        assert(false);
    } // msgBox.exec()
}

bool SendCoinsDialog::signWithExternalSigner(PartiallySignedTransaction& psbtx, CMutableTransaction& mtx, bool& complete) {
    TransactionError err;
    try {
        err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Sign failed"), e.what());
        return false;
    }
    if (err == TransactionError::EXTERNAL_SIGNER_NOT_FOUND) {
        //: "External signer" means using devices such as hardware wallets.
        QMessageBox::critical(nullptr, tr("External signer not found"), "External signer not found");
        return false;
    }
    if (err == TransactionError::EXTERNAL_SIGNER_FAILED) {
        //: "External signer" means using devices such as hardware wallets.
        QMessageBox::critical(nullptr, tr("External signer failure"), "External signer failure");
        return false;
    }
    if (err != TransactionError::OK) {
        tfm::format(std::cerr, "Failed to sign PSBT");
        processSendCoinsReturn(WalletModel::TransactionCreationFailed);
        return false;
    }
    // fillPSBT does not always properly finalize
    complete = FinalizeAndExtractPSBT(psbtx, mtx);
    return true;
}

void SendCoinsDialog::sendButtonClicked([[maybe_unused]] bool checked)
{
    if(!model || !model->getOptionsModel())
        return;

    QString question_string, informative_text, detailed_text;
    if (!PrepareSendText(question_string, informative_text, detailed_text)) return;
    assert(m_current_transaction);
    assert(!g_con_elementsmode || m_current_blind_details);

    const QString confirmation = tr("Confirm Send Coins");
    const bool enable_send{!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner()};
    const bool always_show_unsigned{model->getOptionsModel()->getEnablePSBTControls()};
    auto confirmationDialog = new SendConfirmationDialog(confirmation, question_string, informative_text, detailed_text, SEND_CONFIRM_DELAY, enable_send, always_show_unsigned, this);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    if(retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
        fNewRecipientAllowed = true;
        return;
    }

    bool send_failure = false;
    if (retval == QMessageBox::Save) {
        // "Create Unsigned" clicked
        CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        // Fill without signing
        TransactionError err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
        assert(!complete);
        assert(err == TransactionError::OK);

        // Copy PSBT to clipboard and offer to save
        presentPSBT(psbtx);
    } else {
        // "Send" clicked
        assert(!model->wallet().privateKeysDisabled() || model->wallet().hasExternalSigner());
        bool broadcast = true;
        if (model->wallet().hasExternalSigner()) {
            CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
            PartiallySignedTransaction psbtx(mtx);
            bool complete = false;
            // Always fill without signing first. This prevents an external signer
            // from being called prematurely and is not expensive.
            TransactionError err = model->wallet().fillPSBT(SIGHASH_ALL, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete);
            assert(!complete);
            assert(err == TransactionError::OK);
            send_failure = !signWithExternalSigner(psbtx, mtx, complete);
            // Don't broadcast when user rejects it on the device or there's a failure:
            broadcast = complete && !send_failure;
            if (!send_failure) {
                // A transaction signed with an external signer is not always complete,
                // e.g. in a multisig wallet.
                if (complete) {
                    // Prepare transaction for broadcast transaction if complete
                    const CTransactionRef tx = MakeTransactionRef(mtx);
                    m_current_transaction->setWtx(tx);
                } else {
                    presentPSBT(psbtx);
                }
            }
        }

        // Broadcast the transaction, unless an external signer was used and it
        // failed, or more signatures are needed.
        if (broadcast) {
            // now send the prepared transaction
            WalletModel::SendCoinsReturn sendStatus = model->sendCoins(*m_current_transaction, m_current_blind_details.get());
            // process sendStatus and on error generate message shown to user
            processSendCoinsReturn(sendStatus);

            if (sendStatus.status == WalletModel::OK) {
                Q_EMIT coinsSent(m_current_transaction->getWtx()->GetHash());
            } else {
                send_failure = true;
            }
        }
    }
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        coinControlUpdateLabels();
    }
    fNewRecipientAllowed = true;
    m_current_transaction.reset();
    m_current_blind_details.reset();
}

void SendCoinsDialog::clear()
{
    m_current_transaction.reset();
    m_current_blind_details.reset();

    // Clear coin control settings
    m_coin_control->UnSelectAll();
    ui->checkBoxCoinControlChange->setChecked(false);
    ui->lineEditCoinControlChange->clear();
    coinControlUpdateLabels();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    // A fresh form starts over on the fee asset too: the default follows
    // whatever the user sends next.
    m_fee_asset_user_choice = false;
    updateDefaultFeeAsset();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry *entry = new SendCoinsEntry(platformStyle, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, &SendCoinsEntry::removeEntry, this, &SendCoinsDialog::removeEntry);
    connect(entry, &SendCoinsEntry::useAvailableBalance, this, &SendCoinsDialog::useAvailableBalance);
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
    // The amount field also signals asset switches, so the fee-asset default can
    // follow the asset being sent.
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::updateDefaultFeeAsset);
    connect(entry, &SendCoinsEntry::subtractFeeFromAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
    qApp->processEvents();
    QScrollBar* bar = ui->scrollArea->verticalScrollBar();
    if(bar)
        bar->setSliderPosition(bar->maximum());

    updateTabsAndLabels();
    return entry;
}

void SendCoinsDialog::updateTabsAndLabels()
{
    setupTabChain(nullptr);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString &address)
{
    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(SendAssetsRecipient(rv));
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    // Kept for largestAcceptedHolding(), which ranks the wallet's holdings on
    // every recipient edit and must not recompute balances to do it.
    m_cached_balances = balances;
    if(model && model->getOptionsModel())
    {
        CAmount balance = valueFor(balances.balance, ::policyAsset);
        if (model->wallet().hasExternalSigner()) {
            ui->labelBalanceName->setText(tr("External balance:"));
        } else if (model->wallet().privateKeysDisabled()) {
            balance = valueFor(balances.watch_only_balance, ::policyAsset);
            ui->labelBalanceName->setText(tr("Watch-only balance:"));
        }
        // SEQUENTIA: show the balance valued in the chosen reference currency (≈), like elsewhere.
        QString balText = BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance);
        const QString balRef = GUIUtil::formatReferenceApprox(::policyAsset, balance, model->getOptionsModel()->getReferenceCurrency());
        if (!balRef.isEmpty()) balText += QStringLiteral("  ") + balRef;
        ui->labelBalance->setText(balText);
    }
}

void SendCoinsDialog::updateDisplayUnit()
{
    setBalance(model->wallet().getBalances());
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateSmartFeeLabel();
}

void SendCoinsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // All status values are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getDefaultMaxTxFee()));
        break;
    case WalletModel::PaymentRequestExpired:
        msgParams.first = tr("Payment request expired.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void SendCoinsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void SendCoinsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void SendCoinsDialog::useAvailableBalance(SendCoinsEntry* entry)
{
    // Include watch-only for wallets without private key
    m_coin_control->fAllowWatchOnly = model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner();

    SendAssetsRecipient recipient = entry->getValue();
    // Calculate available amount to send.
    CAmount amount = valueFor(model->wallet().getAvailableBalance(*m_coin_control), recipient.asset);
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* e = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (e && !e->isHidden() && e != entry && e->getValue().asset == recipient.asset) {
            amount -= e->getValue().asset_amount;
        }
    }

    if (amount > 0) {
        if (recipient.asset == ::policyAsset) {
            entry->checkSubtractFeeFromAmount();
        }
        recipient.asset_amount = amount;
    } else {
        recipient.asset_amount = 0;
    }
    entry->setValue(recipient);
}

void SendCoinsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Re-label the per-recipient asset selectors from the registry: a name that
    // resolved after the field was first built would otherwise stay a hex id.
    for (int i = 0; i < ui->entries->count(); ++i) {
        if (auto* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget())) {
            entry->refreshAssetNames();
        }
    }
    // The fee-asset selector holds the same assets by hex data; re-label those too.
    if (g_con_any_asset_fees) {
        for (int i = 0; i < ui->feeAssetSelector->count(); ++i) {
            const CAsset a = GetAssetFromString(ui->feeAssetSelector->itemData(i).toString().toStdString());
            if (!a.IsNull()) ui->feeAssetSelector->setItemText(i, GUIUtil::assetDisplayName(a));
        }
    }
}

void SendCoinsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelCustomFeeWarning   ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked());
}

void SendCoinsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        // The custom-fee field is a rate in the reference unit (that is what
        // m_coin_control->m_feerate carries), while formatFeeRate speaks in the
        // fee asset — so convert first, at the same whitelist rate the mempool
        // will use. The two used to be passed to it interchangeably.
        ui->labelFeeMinimized->setText(formatFeeRate(CFeeRate(ui->customFee->value()).GetFee(1000, selectedFeeAsset())));
    }
}

void SendCoinsDialog::updateCoinControlState()
{
    if (ui->radioCustomFee->isChecked()) {
        m_coin_control->m_feerate = CFeeRate(ui->customFee->value());
    } else {
        m_coin_control->m_feerate.reset();
    }
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    m_coin_control->m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
    m_coin_control->m_signal_bip125_rbf = ui->optInRBF->isChecked();
    // Sequentia: pay the fee in the selected asset. The policy asset is the default and leaves
    // m_fee_asset unset (so the wallet uses the policy asset); any other choice sets it.
    if (g_con_any_asset_fees && ui->feeAssetSelector->count() > 0) {
        const CAsset sel = GetAssetFromString(ui->feeAssetSelector->currentData().toString().toStdString());
        if (!sel.IsNull() && sel != ::policyAsset) {
            m_coin_control->m_fee_asset = sel;
        } else {
            m_coin_control->m_fee_asset.reset();
        }
    }
    // Include watch-only for wallets without private key
    m_coin_control->fAllowWatchOnly = model->wallet().privateKeysDisabled() && !model->wallet().hasExternalSigner();
}

CAsset SendCoinsDialog::largestAcceptedHolding() const
{
    if (!model) return CAsset();
    CAsset best;
    double best_value = 0.0;
    for (int i = 0; i < ui->feeAssetSelector->count(); ++i) {
        const CAsset candidate = GetAssetFromString(ui->feeAssetSelector->itemData(i).toString().toStdString());
        if (candidate.IsNull()) continue;
        const FeeAssetInfo info = model->node().getFeeAssetInfo(candidate);
        if (!info.accepted) continue;
        // Rank by what the holding is WORTH, not by how many atoms of it there
        // are: "10000 of a millionth-of-a-cent token" must not outrank "3 of a
        // dollar one". The whitelist rate is the right yardstick and not merely
        // the convenient one — it is the very valuation this node's mempool will
        // apply to the fee, it is denominated in one unit across every asset, and
        // it exists for exactly the assets that can pay. The display price feed
        // would answer the same question in USD, but only while it is up and only
        // for assets it happens to quote, so it would rank on availability as
        // much as on value.
        const double value = static_cast<double>(valueFor(m_cached_balances.balance, candidate))
                           * static_cast<double>(info.rate) / static_cast<double>(exchange_rate_scale);
        if (value > best_value) { best_value = value; best = candidate; }
    }
    return best;
}

void SendCoinsDialog::updateDefaultFeeAsset()
{
    if (!g_con_any_asset_fees || !model || ui->feeAssetSelector->count() == 0) return;
    if (m_fee_asset_user_choice) { updateFeeAssetWarning(); return; }

    // The asset of the first recipient is the transaction's subject; paying the fee
    // in it is the least surprising default and needs no extra asset in the wallet.
    CAsset sent = ::policyAsset;
    if (ui->entries->count() > 0) {
        if (auto* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget())) {
            sent = entry->sendAsset();
        }
    }

    // ...but only while this node accepts a fee in it. The test is the fee
    // whitelist, not the display price feed: the whitelist is what the mempool
    // consults, so an asset missing from it fails here and now, while an asset
    // missing from the feed merely cannot be shown in dollars. Defaulting on the
    // feed used to do both wrong at once — it offered assets the node refuses and
    // rejected assets it accepts.
    CAsset pick = sent;
    if (!model->node().getFeeAssetInfo(pick).accepted) {
        const CAsset fallback = largestAcceptedHolding();
        // Nothing acceptable in the wallet: leave the selection alone rather than
        // moving it somewhere equally unusable, and let the warning explain.
        if (!fallback.IsNull()) pick = fallback;
    }
    const int idx = ui->feeAssetSelector->findData(QString::fromStdString(pick.GetHex()));
    if (idx >= 0 && idx != ui->feeAssetSelector->currentIndex()) {
        ui->feeAssetSelector->setCurrentIndex(idx);
    }
    updateFeeAssetWarning();
}

CAsset SendCoinsDialog::selectedFeeAsset() const
{
    if (!g_con_any_asset_fees || ui->feeAssetSelector->count() == 0) return ::policyAsset;
    const CAsset sel = GetAssetFromString(ui->feeAssetSelector->currentData().toString().toStdString());
    return sel.IsNull() ? ::policyAsset : sel;
}

QString SendCoinsDialog::formatFeeRate(const CAmount& fee_asset_atoms_per_kvb) const
{
    // The figure arrives already denominated in the selected fee asset:
    // wallet::GetMinimumFee() ends in GetFee(bytes, coin_control.m_fee_asset),
    // which converts out of the reference unit at this node's whitelist rate.
    // So the asset amount is the fact, and it is the number the user will pay;
    // the reference currency is the aid that makes it legible. Reading the
    // figure as policy-asset atoms and converting it into the fee asset a
    // second time — at display-feed prices, no less — quoted a fee that was
    // wrong by the ratio between the two rates whenever the two assets differed.
    const QString ref = GUIUtil::referenceCurrency();
    const CAsset feeAsset = selectedFeeAsset();

    // A fee asset this node does not accept converts to zero, and "0/kvB" reads
    // as "free" rather than "impossible". The warning label says why.
    if (g_con_any_asset_fees && model && !model->node().getFeeAssetInfo(feeAsset).accepted) {
        return tr("unavailable — this node does not accept fees in %1").arg(GUIUtil::assetDisplayName(feeAsset));
    }

    QStringList parts;
    parts << GUIUtil::formatAssetAmount(feeAsset, fee_asset_atoms_per_kvb, BitcoinUnits::BTC,
                                        BitcoinUnits::SeparatorStyle::STANDARD, /*include_asset_name=*/true)
             + QStringLiteral("/kvB");

    double refValue = 0.0;
    if (GUIUtil::referenceValueOf(feeAsset, fee_asset_atoms_per_kvb, ref, refValue)) {
        QString line = GUIUtil::formatReferenceAmount(refValue, ref) + QStringLiteral("/kvB");
        // The unit price behind it, so the conversion is checkable rather than magic.
        double unitValue = 0.0;
        if (GUIUtil::unitReferenceValue(feeAsset, ref, unitValue)) {
            line += QStringLiteral(" <span style='color:#888'>(1 ") + GUIUtil::assetDisplayName(feeAsset)
                  + QString::fromUtf8(" \xE2\x89\x88 ") + GUIUtil::formatReferenceAmount(unitValue, ref) + QStringLiteral(")</span>");
        }
        parts << line;
    }
    return parts.join(QStringLiteral(" = "));
}

void SendCoinsDialog::updateFeeAssetWarning()
{
    if (!g_con_any_asset_fees || !model || ui->feeAssetSelector->count() == 0) {
        ui->labelFeeAssetWarning->setVisible(false);
        return;
    }
    const CAsset sel = GetAssetFromString(ui->feeAssetSelector->currentData().toString().toStdString());
    if (sel.IsNull()) {
        ui->labelFeeAssetWarning->setVisible(false);
        return;
    }
    const FeeAssetInfo info = model->node().getFeeAssetInfo(sel);
    const QString name = GUIUtil::assetDisplayName(sel);

    // Three different things can be wrong with a fee asset and they are not the
    // same warning. Worst first.
    //
    // 1. This node does not accept it. Nothing downstream matters: the wallet's
    //    own mempool refuses the transaction, so it never reaches a producer.
    // 2. This node accepts it but the wider network probably will not, because
    //    the registry does not publish it or no feed prices it — the two inputs
    //    every price server uses to build its whitelist.
    // 3. Nothing is wrong. Not being able to show the fee in dollars is not a
    //    problem with the fee; it stays silent.
    if (!info.accepted) {
        // Being listed at rate 0 and being absent are the same refusal but not
        // the same mistake: one is a policy someone wrote down, the other is an
        // asset nobody has configured. Saying which saves the search.
        const QString why = info.listed
            ? tr("This node's fee policy lists %1 at rate 0, which refuses it.").arg(name)
            : tr("%1 is not in this node's fee whitelist.").arg(name);
        // Red, not amber: this one is not a risk to weigh, it is a transaction
        // that cannot be sent.
        ui->labelFeeAssetWarning->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        ui->labelFeeAssetWarning->setText(
            why + QLatin1Char(' ') +
            tr("The transaction would be rejected by your own node before it ever reached a block "
               "producer. Pick an accepted asset, or set a rate for %1 under Settings → Fee policy.").arg(name));
        ui->labelFeeAssetWarning->setVisible(true);
        return;
    }

    if (!info.registry_listed || !info.has_market_price) {
        const QString why = !info.registry_listed
            ? tr("%1 is not published on the Asset Registry, so the price servers other block producers "
                 "run will not discover it.").arg(name)
            : tr("No published market price for %1, so other block producers' price servers cannot "
                 "value it.").arg(name);
        // Replace-By-Fee is the remedy here and only here: this transaction is
        // valid and relayable, it may simply sit unconfirmed, and the fix is to
        // switch the fee to a better-travelled asset later — which is precisely
        // what RBF allows.
        const QString remedy = ui->optInRBF->isChecked()
            ? tr("Replace-By-Fee is on, so you can switch the fee to another asset later if it does not confirm.")
            : tr("Turn on Replace-By-Fee below so you can switch the fee to another asset later if it does not confirm.");
        ui->labelFeeAssetWarning->setStyleSheet(QStringLiteral("color: #ffb84d;"));
        ui->labelFeeAssetWarning->setText(
            why + QLatin1Char(' ') +
            tr("This node accepts it, so the payment may confirm only in a block this node produces.") +
            QLatin1Char(' ') + remedy);
        ui->labelFeeAssetWarning->setVisible(true);
        return;
    }

    ui->labelFeeAssetWarning->setVisible(false);
}

void SendCoinsDialog::updateNumberOfBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers, SynchronizationState sync_state) {
    if (sync_state == SynchronizationState::POST_INIT) {
        updateSmartFeeLabel();
    }
}

void SendCoinsDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    updateCoinControlState();
    m_coin_control->m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels
    int returned_target;
    FeeReason reason;
    CFeeRate feeRate = CFeeRate(model->wallet().getMinimumFee(1000, *m_coin_control, &returned_target, &reason));

    ui->labelSmartFee->setText(formatFeeRate(feeRate.GetFeePerK()));

    if (reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
        ui->fallbackFeeWarningLabel->setIndent(GUIUtil::TextWidth(QFontMetrics(ui->fallbackFeeWarningLabel->font()), "x"));
    }
    else
    {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", returned_target));
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void SendCoinsDialog::coinControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) { // coin control features disabled
        m_coin_control = std::make_unique<CCoinControl>();
    }

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    auto dlg = new CoinControlDialog(*m_coin_control, model, platformStyle);
    connect(dlg, &QDialog::finished, this, &SendCoinsDialog::coinControlUpdateLabels);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        // ELEMENTS: it's a map now, should be initialized automatically
        //m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        // ELEMENTS: it's a map now, should be initialized automatically
        //m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid address"));
        }
        else // Valid address
        {
            if (!model->wallet().isSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes) {
                    // ELEMENTS: it's a map now
                    //m_coin_control->destChange = dest;
                }
                else
                {
                    ui->lineEditCoinControlChange->setText("");
                    ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    ui->labelCoinControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                // ELEMENTS: it's a map now
                //m_coin_control->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState();

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendAssetsRecipient rcp = entry->getValue();
            if (rcp.asset == Params().GetConsensus().pegged_asset) {
                CoinControlDialog::payAmounts.append(rcp.asset_amount);
            }
            if (rcp.fSubtractFeeFromAmount)
                CoinControlDialog::fSubtractFeeFromAmount = true;
        }
    }

    if (m_coin_control->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(*m_coin_control, model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

SendConfirmationDialog::SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text, const QString& detailed_text, int _secDelay, bool enable_send, bool always_show_unsigned, QWidget* parent)
    : QMessageBox(parent), secDelay(_secDelay), m_enable_send(enable_send)
{
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setDetailedText(detailed_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    if (always_show_unsigned || !enable_send) addButton(QMessageBox::Save);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    if (confirmButtonText.isEmpty()) {
        confirmButtonText = yesButton->text();
    }
    m_psbt_button = button(QMessageBox::Save);
    updateButtons();
    connect(&countDownTimer, &QTimer::timeout, this, &SendConfirmationDialog::countDown);
}

int SendConfirmationDialog::exec()
{
    updateButtons();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateButtons();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateButtons()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + (m_enable_send ? (" (" + QString::number(secDelay) + ")") : QString("")));
        if (m_psbt_button) {
            m_psbt_button->setEnabled(false);
            m_psbt_button->setText(m_psbt_button_text + " (" + QString::number(secDelay) + ")");
        }
    }
    else
    {
        yesButton->setEnabled(m_enable_send);
        yesButton->setText(confirmButtonText);
        if (m_psbt_button) {
            m_psbt_button->setEnabled(true);
            m_psbt_button->setText(m_psbt_button_text);
        }
    }
}
