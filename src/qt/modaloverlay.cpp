// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/modaloverlay.h>
#include <qt/forms/ui_modaloverlay.h>

#include <chainparams.h>
#include <qt/guiutil.h>

#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QResizeEvent>

ModalOverlay::ModalOverlay(bool enable_wallet, QWidget *parent) :
QWidget(parent),
ui(new Ui::ModalOverlay),
bestHeaderHeight(0),
bestHeaderDate(QDateTime()),
layerIsVisible(false),
userClosed(false)
{
    ui->setupUi(this);
    // A wrapping QLabel knows how tall it must be at a given width, but the
    // layout only asks when the size policy says to — otherwise it allocates
    // the plain size hint and the last line is cut off mid-stroke. Fixing the
    // panel width (see modaloverlay.ui) was not enough on its own.
    for (QLabel* l : {ui->infoText, ui->infoTextStrong}) {
        QSizePolicy sp = l->sizePolicy();
        sp.setHeightForWidth(true);
        sp.setVerticalPolicy(QSizePolicy::MinimumExpanding);
        l->setSizePolicy(sp);
    }
    connect(ui->closeButton, &QPushButton::clicked, this, &ModalOverlay::closeClicked);
    if (parent) {
        parent->installEventFilter(this);
        raise();
    }

    blockProcessTime.clear();
    setVisible(false);
    if (!enable_wallet) {
        ui->infoText->setVisible(false);
        ui->infoTextStrong->setText(tr("%1 is currently syncing.  It will download headers and blocks from peers and validate them until reaching the tip of the block chain.").arg(PACKAGE_NAME));
    }

    m_animation.setTargetObject(this);
    m_animation.setPropertyName("pos");
    m_animation.setDuration(300 /* ms */);
    m_animation.setEasingCurve(QEasingCurve::OutQuad);
}

ModalOverlay::~ModalOverlay()
{
    delete ui;
}

bool ModalOverlay::eventFilter(QObject * obj, QEvent * ev) {
    if (obj == parent()) {
        if (ev->type() == QEvent::Resize) {
            QResizeEvent * rev = static_cast<QResizeEvent*>(ev);
            resize(rev->size());
            if (!layerIsVisible)
                setGeometry(0, height(), width(), height());

            if (m_animation.endValue().toPoint().y() > 0) {
                m_animation.setEndValue(QPoint(0, height()));
            }
        }
        else if (ev->type() == QEvent::ChildAdded) {
            raise();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

//! Tracks parent widget changes
bool ModalOverlay::event(QEvent* ev) {
    if (ev->type() == QEvent::ParentAboutToChange) {
        if (parent()) parent()->removeEventFilter(this);
    }
    else if (ev->type() == QEvent::ParentChange) {
        if (parent()) {
            parent()->installEventFilter(this);
            raise();
        }
    }
    return QWidget::event(ev);
}

void ModalOverlay::setKnownBestHeight(int count, const QDateTime& blockDate)
{
    if (count > bestHeaderHeight) {
        bestHeaderHeight = count;
        bestHeaderDate = blockDate;
        // SEQUENTIA: feed the spacing estimator (see GUIUtil::HeaderSyncEstimator).
        m_header_estimator.AddSample(count, blockDate.toSecsSinceEpoch());
        UpdateHeaderSyncLabel();
    }
}

void ModalOverlay::tipUpdate(int count, const QDateTime& blockDate, double nVerificationProgress)
{
    QDateTime currentDate = QDateTime::currentDateTime();

    // keep a vector of samples of verification progress at height
    blockProcessTime.push_front(qMakePair(currentDate.toMSecsSinceEpoch(), nVerificationProgress));

    // show progress speed if we have more than one sample
    if (blockProcessTime.size() >= 2) {
        double progressDelta = 0;
        double progressPerHour = 0;
        qint64 timeDelta = 0;
        qint64 remainingMSecs = 0;
        double remainingProgress = 1.0 - nVerificationProgress;
        for (int i = 1; i < blockProcessTime.size(); i++) {
            QPair<qint64, double> sample = blockProcessTime[i];

            // take first sample after 500 seconds or last available one
            if (sample.first < (currentDate.toMSecsSinceEpoch() - 500 * 1000) || i == blockProcessTime.size() - 1) {
                progressDelta = blockProcessTime[0].second - sample.second;
                timeDelta = blockProcessTime[0].first - sample.first;
                progressPerHour = (progressDelta > 0) ? progressDelta / (double)timeDelta * 1000 * 3600 : 0;
                remainingMSecs = (progressDelta > 0) ? remainingProgress / progressDelta * timeDelta : -1;
                break;
            }
        }
        // show expected remaining time
        if(remainingMSecs >= 0) {
            ui->expectedTimeLeft->setText(GUIUtil::formatNiceTimeOffset(remainingMSecs / 1000.0));
        } else {
            ui->expectedTimeLeft->setText(QObject::tr("unknown"));
        }

        static const int MAX_SAMPLES = 5000;
        if (blockProcessTime.count() > MAX_SAMPLES) {
            blockProcessTime.remove(MAX_SAMPLES, blockProcessTime.count() - MAX_SAMPLES);
        }
    }

    // show the last block date
    ui->newestBlockDate->setText(blockDate.toString());

    // show the percentage done according to nVerificationProgress
    ui->percentageProgress->setText(QString::number(nVerificationProgress*100, 'f', 2)+"%");

    if (!bestHeaderDate.isValid())
        // not syncing
        return;

    // estimate the number of headers left from the chain's measured spacing
    // and check if the gui is not aware of the best header (happens rarely)
    int estimateNumHeadersLeft = EstimatedHeadersLeft();
    bool hasBestHeader = bestHeaderHeight >= count;

    // show remaining number of blocks
    if (estimateNumHeadersLeft < HEADER_HEIGHT_DELTA_SYNC && hasBestHeader) {
        ui->numberOfBlocksLeft->setText(QString::number(bestHeaderHeight - count));
    } else {
        UpdateHeaderSyncLabel();
        ui->expectedTimeLeft->setText(tr("Unknown…"));
    }
}

int ModalOverlay::EstimatedHeadersLeft() const
{
    const int64_t now = QDateTime::currentSecsSinceEpoch();
    const int measured = m_header_estimator.HeadersLeft(now, Params().GetConsensus().nPowTargetSpacing);
    if (measured >= 0) return measured;
    // Not calibrated yet: fall back to upstream's consensus-spacing estimate.
    if (!bestHeaderDate.isValid()) return 0;
    return bestHeaderDate.secsTo(QDateTime::currentDateTime()) / Params().GetConsensus().nPowTargetSpacing;
}

void ModalOverlay::UpdateHeaderSyncLabel() {
    int est_headers_left = EstimatedHeadersLeft();

    // SEQUENTIA: show how far this session's header sync has come, not how much
    // of the chain since genesis we hold. Upstream's ratio counts from genesis,
    // so a node restarting with most headers already on disk opens near 100%
    // and the download that actually remains is invisible. Rebasing on the
    // height we started from makes the run 0→100% over the headers still to
    // fetch, matching the block phase that follows.
    if (m_header_sync_start_height < 0) m_header_sync_start_height = bestHeaderHeight;
    const int target = bestHeaderHeight + est_headers_left;
    double pct = 0.0;
    if (target > m_header_sync_start_height) {
        pct = 100.0 * double(bestHeaderHeight - m_header_sync_start_height) /
                      double(target - m_header_sync_start_height);
        pct = qBound(0.0, pct, 100.0);
    }
    ui->numberOfBlocksLeft->setText(tr("Unknown. Syncing Headers (%1, %2%)…").arg(bestHeaderHeight).arg(QString::number(pct, 'f', 1)));
}

void ModalOverlay::toggleVisibility()
{
    showHide(layerIsVisible, true);
    if (!layerIsVisible)
        userClosed = true;
}

void ModalOverlay::showHide(bool hide, bool userRequested)
{
    if ( (layerIsVisible && !hide) || (!layerIsVisible && hide) || (!hide && userClosed && !userRequested))
        return;

    Q_EMIT triggered(hide);

    if (!isVisible() && !hide)
        setVisible(true);

    m_animation.setStartValue(QPoint(0, hide ? 0 : height()));
    // The eventFilter() updates the endValue if it is required for QEvent::Resize.
    m_animation.setEndValue(QPoint(0, hide ? height() : 0));
    m_animation.start(QAbstractAnimation::KeepWhenStopped);
    layerIsVisible = !hide;
}

void ModalOverlay::closeClicked()
{
    showHide(true);
    userClosed = true;
}
