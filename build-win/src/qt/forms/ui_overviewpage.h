/********************************************************************************
** Form generated from reading UI file 'overviewpage.ui'
**
** Created by: Qt User Interface Compiler version 5.15.3
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_OVERVIEWPAGE_H
#define UI_OVERVIEWPAGE_H

#include <QtCore/QVariant>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include "qt/transactionoverviewwidget.h"

QT_BEGIN_NAMESPACE

class Ui_OverviewPage
{
public:
    QVBoxLayout *topLayout;
    QLabel *labelAlerts;
    QHBoxLayout *horizontalLayout;
    QVBoxLayout *verticalLayout_2;
    QFrame *frame;
    QVBoxLayout *verticalLayout_4;
    QHBoxLayout *horizontalLayout_4;
    QLabel *label_5;
    QPushButton *labelWalletStatus;
    QSpacerItem *horizontalSpacer_3;
    QScrollArea *scrollArea;
    QWidget *scrollAreaWidgetContents;
    QWidget *widget;
    QGridLayout *gridLayout;
    QFrame *line;
    QLabel *labelSpendable;
    QLabel *labelWatchAvailable;
    QLabel *labelImmatureText;
    QLabel *labelTotalText;
    QLabel *labelBalanceText;
    QLabel *labelUnconfirmed;
    QLabel *labelWatchonly;
    QLabel *labelImmature;
    QLabel *labelPendingText;
    QSpacerItem *horizontalSpacer_2;
    QFrame *lineWatchBalance;
    QLabel *labelWatchTotal;
    QLabel *labelTotal;
    QLabel *labelWatchPending;
    QLabel *labelWatchImmature;
    QLabel *labelBalance;
    QVBoxLayout *verticalLayout_3;
    QFrame *frame_2;
    QVBoxLayout *verticalLayout;
    QHBoxLayout *horizontalLayout_2;
    QLabel *label_4;
    QPushButton *labelTransactionsStatus;
    QSpacerItem *horizontalSpacer;
    TransactionOverviewWidget *listTransactions;

    void setupUi(QWidget *OverviewPage)
    {
        if (OverviewPage->objectName().isEmpty())
            OverviewPage->setObjectName(QString::fromUtf8("OverviewPage"));
        OverviewPage->resize(798, 781);
        topLayout = new QVBoxLayout(OverviewPage);
        topLayout->setObjectName(QString::fromUtf8("topLayout"));
        labelAlerts = new QLabel(OverviewPage);
        labelAlerts->setObjectName(QString::fromUtf8("labelAlerts"));
        labelAlerts->setVisible(true);
        labelAlerts->setStyleSheet(QString::fromUtf8("QLabel { background-color: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 0, stop:0 #F0D0A0, stop:1 #F8D488); color:#000000; }"));
        labelAlerts->setWordWrap(true);
        labelAlerts->setMargin(3);
        labelAlerts->setTextInteractionFlags(Qt::TextSelectableByMouse);

        topLayout->addWidget(labelAlerts);

        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName(QString::fromUtf8("horizontalLayout"));
        verticalLayout_2 = new QVBoxLayout();
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        frame = new QFrame(OverviewPage);
        frame->setObjectName(QString::fromUtf8("frame"));
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setFrameShadow(QFrame::Raised);
        verticalLayout_4 = new QVBoxLayout(frame);
        verticalLayout_4->setObjectName(QString::fromUtf8("verticalLayout_4"));
        horizontalLayout_4 = new QHBoxLayout();
        horizontalLayout_4->setObjectName(QString::fromUtf8("horizontalLayout_4"));
        label_5 = new QLabel(frame);
        label_5->setObjectName(QString::fromUtf8("label_5"));
        QFont font;
        font.setBold(true);
        font.setWeight(75);
        label_5->setFont(font);

        horizontalLayout_4->addWidget(label_5);

        labelWalletStatus = new QPushButton(frame);
        labelWalletStatus->setObjectName(QString::fromUtf8("labelWalletStatus"));
        labelWalletStatus->setEnabled(true);
        labelWalletStatus->setMaximumSize(QSize(45, 16777215));
        QIcon icon;
        icon.addFile(QString::fromUtf8(":/icons/warning"), QSize(), QIcon::Normal, QIcon::Off);
        icon.addFile(QString::fromUtf8(":/icons/warning"), QSize(), QIcon::Disabled, QIcon::Off);
        labelWalletStatus->setIcon(icon);
        labelWalletStatus->setIconSize(QSize(24, 24));

        horizontalLayout_4->addWidget(labelWalletStatus);

        horizontalSpacer_3 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_4->addItem(horizontalSpacer_3);


        verticalLayout_4->addLayout(horizontalLayout_4);

        scrollArea = new QScrollArea(frame);
        scrollArea->setObjectName(QString::fromUtf8("scrollArea"));
        QSizePolicy sizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(scrollArea->sizePolicy().hasHeightForWidth());
        scrollArea->setSizePolicy(sizePolicy);
        scrollArea->setMinimumSize(QSize(500, 300));
        scrollArea->setSizeIncrement(QSize(1, 1));
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
        scrollArea->setWidgetResizable(true);
        scrollAreaWidgetContents = new QWidget();
        scrollAreaWidgetContents->setObjectName(QString::fromUtf8("scrollAreaWidgetContents"));
        scrollAreaWidgetContents->setGeometry(QRect(0, 0, 560, 296));
        sizePolicy.setHeightForWidth(scrollAreaWidgetContents->sizePolicy().hasHeightForWidth());
        scrollAreaWidgetContents->setSizePolicy(sizePolicy);
        scrollAreaWidgetContents->setMinimumSize(QSize(500, 200));
        widget = new QWidget(scrollAreaWidgetContents);
        widget->setObjectName(QString::fromUtf8("widget"));
        widget->setGeometry(QRect(0, 0, 551, 253));
        gridLayout = new QGridLayout(widget);
        gridLayout->setSpacing(12);
        gridLayout->setObjectName(QString::fromUtf8("gridLayout"));
        gridLayout->setSizeConstraint(QLayout::SetMinimumSize);
        gridLayout->setContentsMargins(0, 0, 0, 0);
        line = new QFrame(widget);
        line->setObjectName(QString::fromUtf8("line"));
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);

        gridLayout->addWidget(line, 4, 0, 1, 2);

        labelSpendable = new QLabel(widget);
        labelSpendable->setObjectName(QString::fromUtf8("labelSpendable"));
        labelSpendable->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

        gridLayout->addWidget(labelSpendable, 0, 1, 1, 1);

        labelWatchAvailable = new QLabel(widget);
        labelWatchAvailable->setObjectName(QString::fromUtf8("labelWatchAvailable"));
        sizePolicy.setHeightForWidth(labelWatchAvailable->sizePolicy().hasHeightForWidth());
        labelWatchAvailable->setSizePolicy(sizePolicy);
        labelWatchAvailable->setCursor(QCursor(Qt::IBeamCursor));
        labelWatchAvailable->setText(QString::fromUtf8("0.00000000 BTC"));
        labelWatchAvailable->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelWatchAvailable->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelWatchAvailable, 1, 2, 1, 1);

        labelImmatureText = new QLabel(widget);
        labelImmatureText->setObjectName(QString::fromUtf8("labelImmatureText"));
        sizePolicy.setHeightForWidth(labelImmatureText->sizePolicy().hasHeightForWidth());
        labelImmatureText->setSizePolicy(sizePolicy);
        labelImmatureText->setAlignment(Qt::AlignLeading|Qt::AlignLeft|Qt::AlignTop);

        gridLayout->addWidget(labelImmatureText, 3, 0, 1, 1);

        labelTotalText = new QLabel(widget);
        labelTotalText->setObjectName(QString::fromUtf8("labelTotalText"));
        labelTotalText->setAlignment(Qt::AlignLeading|Qt::AlignLeft|Qt::AlignTop);

        gridLayout->addWidget(labelTotalText, 5, 0, 1, 1);

        labelBalanceText = new QLabel(widget);
        labelBalanceText->setObjectName(QString::fromUtf8("labelBalanceText"));
        labelBalanceText->setAlignment(Qt::AlignLeading|Qt::AlignLeft|Qt::AlignTop);

        gridLayout->addWidget(labelBalanceText, 1, 0, 1, 1);

        labelUnconfirmed = new QLabel(widget);
        labelUnconfirmed->setObjectName(QString::fromUtf8("labelUnconfirmed"));
        sizePolicy.setHeightForWidth(labelUnconfirmed->sizePolicy().hasHeightForWidth());
        labelUnconfirmed->setSizePolicy(sizePolicy);
        labelUnconfirmed->setCursor(QCursor(Qt::IBeamCursor));
        labelUnconfirmed->setText(QString::fromUtf8("0.00000000 BTC"));
        labelUnconfirmed->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelUnconfirmed->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelUnconfirmed, 2, 1, 1, 1);

        labelWatchonly = new QLabel(widget);
        labelWatchonly->setObjectName(QString::fromUtf8("labelWatchonly"));
        labelWatchonly->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);

        gridLayout->addWidget(labelWatchonly, 0, 2, 1, 1);

        labelImmature = new QLabel(widget);
        labelImmature->setObjectName(QString::fromUtf8("labelImmature"));
        sizePolicy.setHeightForWidth(labelImmature->sizePolicy().hasHeightForWidth());
        labelImmature->setSizePolicy(sizePolicy);
        labelImmature->setCursor(QCursor(Qt::IBeamCursor));
        labelImmature->setText(QString::fromUtf8("0.00000000 BTC"));
        labelImmature->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelImmature->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelImmature, 3, 1, 1, 1);

        labelPendingText = new QLabel(widget);
        labelPendingText->setObjectName(QString::fromUtf8("labelPendingText"));
        labelPendingText->setAlignment(Qt::AlignLeading|Qt::AlignLeft|Qt::AlignTop);

        gridLayout->addWidget(labelPendingText, 2, 0, 1, 1);

        horizontalSpacer_2 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        gridLayout->addItem(horizontalSpacer_2, 2, 3, 1, 1);

        lineWatchBalance = new QFrame(widget);
        lineWatchBalance->setObjectName(QString::fromUtf8("lineWatchBalance"));
        QSizePolicy sizePolicy1(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sizePolicy1.setHorizontalStretch(0);
        sizePolicy1.setVerticalStretch(0);
        sizePolicy1.setHeightForWidth(lineWatchBalance->sizePolicy().hasHeightForWidth());
        lineWatchBalance->setSizePolicy(sizePolicy1);
        lineWatchBalance->setMinimumSize(QSize(140, 0));
        lineWatchBalance->setFrameShape(QFrame::HLine);
        lineWatchBalance->setFrameShadow(QFrame::Sunken);

        gridLayout->addWidget(lineWatchBalance, 4, 2, 1, 1);

        labelWatchTotal = new QLabel(widget);
        labelWatchTotal->setObjectName(QString::fromUtf8("labelWatchTotal"));
        labelWatchTotal->setCursor(QCursor(Qt::IBeamCursor));
        labelWatchTotal->setText(QString::fromUtf8("0.00000000 BTC"));
        labelWatchTotal->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelWatchTotal->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelWatchTotal, 5, 2, 1, 1);

        labelTotal = new QLabel(widget);
        labelTotal->setObjectName(QString::fromUtf8("labelTotal"));
        sizePolicy.setHeightForWidth(labelTotal->sizePolicy().hasHeightForWidth());
        labelTotal->setSizePolicy(sizePolicy);
        labelTotal->setCursor(QCursor(Qt::IBeamCursor));
        labelTotal->setText(QString::fromUtf8("0.00000000 BTC"));
        labelTotal->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelTotal->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelTotal, 5, 1, 1, 1);

        labelWatchPending = new QLabel(widget);
        labelWatchPending->setObjectName(QString::fromUtf8("labelWatchPending"));
        labelWatchPending->setCursor(QCursor(Qt::IBeamCursor));
        labelWatchPending->setText(QString::fromUtf8("0.00000000 BTC"));
        labelWatchPending->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelWatchPending->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelWatchPending, 2, 2, 1, 1);

        labelWatchImmature = new QLabel(widget);
        labelWatchImmature->setObjectName(QString::fromUtf8("labelWatchImmature"));
        sizePolicy.setHeightForWidth(labelWatchImmature->sizePolicy().hasHeightForWidth());
        labelWatchImmature->setSizePolicy(sizePolicy);
        labelWatchImmature->setCursor(QCursor(Qt::IBeamCursor));
        labelWatchImmature->setText(QString::fromUtf8("0.00000000 BTC"));
        labelWatchImmature->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelWatchImmature->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelWatchImmature, 3, 2, 1, 1);

        labelBalance = new QLabel(widget);
        labelBalance->setObjectName(QString::fromUtf8("labelBalance"));
        sizePolicy.setHeightForWidth(labelBalance->sizePolicy().hasHeightForWidth());
        labelBalance->setSizePolicy(sizePolicy);
        labelBalance->setCursor(QCursor(Qt::IBeamCursor));
        labelBalance->setText(QString::fromUtf8("0.00000000 BTC"));
        labelBalance->setAlignment(Qt::AlignRight|Qt::AlignTrailing|Qt::AlignVCenter);
        labelBalance->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::TextSelectableByKeyboard|Qt::TextSelectableByMouse);

        gridLayout->addWidget(labelBalance, 1, 1, 1, 1);

        scrollArea->setWidget(scrollAreaWidgetContents);

        verticalLayout_4->addWidget(scrollArea);


        verticalLayout_2->addWidget(frame);

        verticalLayout_3 = new QVBoxLayout();
        verticalLayout_3->setObjectName(QString::fromUtf8("verticalLayout_3"));
        frame_2 = new QFrame(OverviewPage);
        frame_2->setObjectName(QString::fromUtf8("frame_2"));
        frame_2->setFrameShape(QFrame::StyledPanel);
        frame_2->setFrameShadow(QFrame::Raised);
        verticalLayout = new QVBoxLayout(frame_2);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setObjectName(QString::fromUtf8("horizontalLayout_2"));
        label_4 = new QLabel(frame_2);
        label_4->setObjectName(QString::fromUtf8("label_4"));
        label_4->setFont(font);

        horizontalLayout_2->addWidget(label_4);

        labelTransactionsStatus = new QPushButton(frame_2);
        labelTransactionsStatus->setObjectName(QString::fromUtf8("labelTransactionsStatus"));
        labelTransactionsStatus->setEnabled(true);
        labelTransactionsStatus->setMaximumSize(QSize(30, 16777215));
        labelTransactionsStatus->setIcon(icon);
        labelTransactionsStatus->setIconSize(QSize(24, 24));
        labelTransactionsStatus->setFlat(true);

        horizontalLayout_2->addWidget(labelTransactionsStatus);

        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout_2->addItem(horizontalSpacer);


        verticalLayout->addLayout(horizontalLayout_2);

        listTransactions = new TransactionOverviewWidget(frame_2);
        listTransactions->setObjectName(QString::fromUtf8("listTransactions"));
        listTransactions->setStyleSheet(QString::fromUtf8("QListView { background: transparent; }"));
        listTransactions->setFrameShape(QFrame::NoFrame);
        listTransactions->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        listTransactions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        listTransactions->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
        listTransactions->setSelectionMode(QAbstractItemView::NoSelection);

        verticalLayout->addWidget(listTransactions);


        verticalLayout_3->addWidget(frame_2);


        verticalLayout_2->addLayout(verticalLayout_3);


        horizontalLayout->addLayout(verticalLayout_2);

        horizontalLayout->setStretch(0, 1);

        topLayout->addLayout(horizontalLayout);


        retranslateUi(OverviewPage);

        QMetaObject::connectSlotsByName(OverviewPage);
    } // setupUi

    void retranslateUi(QWidget *OverviewPage)
    {
        OverviewPage->setWindowTitle(QCoreApplication::translate("OverviewPage", "Form", nullptr));
        label_5->setText(QCoreApplication::translate("OverviewPage", "Balances", nullptr));
#if QT_CONFIG(tooltip)
        labelWalletStatus->setToolTip(QCoreApplication::translate("OverviewPage", "The displayed information may be out of date. Your wallet automatically synchronizes with the Sequentia network after a connection is established, but this process has not completed yet.", nullptr));
#endif // QT_CONFIG(tooltip)
        labelWalletStatus->setText(QString());
        labelSpendable->setText(QCoreApplication::translate("OverviewPage", "Spendable:", nullptr));
#if QT_CONFIG(tooltip)
        labelWatchAvailable->setToolTip(QCoreApplication::translate("OverviewPage", "Your current balance in watch-only addresses", nullptr));
#endif // QT_CONFIG(tooltip)
        labelImmatureText->setText(QCoreApplication::translate("OverviewPage", "Immature:", nullptr));
        labelTotalText->setText(QCoreApplication::translate("OverviewPage", "Total:", nullptr));
        labelBalanceText->setText(QCoreApplication::translate("OverviewPage", "Available:", nullptr));
#if QT_CONFIG(tooltip)
        labelUnconfirmed->setToolTip(QCoreApplication::translate("OverviewPage", "Total of transactions that have yet to be confirmed, and do not yet count toward the spendable balance", nullptr));
#endif // QT_CONFIG(tooltip)
        labelWatchonly->setText(QCoreApplication::translate("OverviewPage", "Watch-only:", nullptr));
#if QT_CONFIG(tooltip)
        labelImmature->setToolTip(QCoreApplication::translate("OverviewPage", "Mined balance that has not yet matured", nullptr));
#endif // QT_CONFIG(tooltip)
        labelPendingText->setText(QCoreApplication::translate("OverviewPage", "Pending:", nullptr));
#if QT_CONFIG(tooltip)
        labelWatchTotal->setToolTip(QCoreApplication::translate("OverviewPage", "Current total balance in watch-only addresses", nullptr));
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        labelTotal->setToolTip(QCoreApplication::translate("OverviewPage", "Your current total balance", nullptr));
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        labelWatchPending->setToolTip(QCoreApplication::translate("OverviewPage", "Unconfirmed transactions to watch-only addresses", nullptr));
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        labelWatchImmature->setToolTip(QCoreApplication::translate("OverviewPage", "Mined balance in watch-only addresses that has not yet matured", nullptr));
#endif // QT_CONFIG(tooltip)
#if QT_CONFIG(tooltip)
        labelBalance->setToolTip(QCoreApplication::translate("OverviewPage", "Your current spendable balance", nullptr));
#endif // QT_CONFIG(tooltip)
        label_4->setText(QCoreApplication::translate("OverviewPage", "Recent transactions", nullptr));
#if QT_CONFIG(tooltip)
        labelTransactionsStatus->setToolTip(QCoreApplication::translate("OverviewPage", "The displayed information may be out of date. Your wallet automatically synchronizes with the Sequentia network after a connection is established, but this process has not completed yet.", nullptr));
#endif // QT_CONFIG(tooltip)
        labelTransactionsStatus->setText(QString());
    } // retranslateUi

};

namespace Ui {
    class OverviewPage: public Ui_OverviewPage {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_OVERVIEWPAGE_H
