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
#include <QShowEvent>
#include <QTimer>

ModalOverlay::ModalOverlay(bool enable_wallet, QWidget *parent) :
QWidget(parent),
ui(new Ui::ModalOverlay),
bestHeaderHeight(0),
bestHeaderDate(QDateTime()),
layerIsVisible(false),
userClosed(false)
{
    ui->setupUi(this);
    // A wrapping QLabel is given the height of its plain size hint, which is
    // not the height its text actually occupies once wrapped — so the last line
    // was cut off mid-stroke. Asking the layout to honour heightForWidth is not
    // enough here (tried, still clipped), so once the real width is known the
    // height the text needs is set as a floor. See FitInfoText().
    for (QLabel* l : {ui->infoText, ui->infoTextStrong}) {
        QSizePolicy sp = l->sizePolicy();
        sp.setHeightForWidth(true);
        // Exactly the height the text needs — not more. An expanding policy here
        // makes the paragraphs swallow the spare space and leaves the panel full
        // of gaps.
        sp.setVerticalPolicy(QSizePolicy::Minimum);
        l->setSizePolicy(sp);
    }
    // "Number of blocks left" carries a sentence while headers are still coming
    // in ("Unknown. Syncing headers…"), which ran off the panel's fixed width.
    ui->numberOfBlocksLeft->setWordWrap(true);
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
    const qint64 now = currentDate.toMSecsSinceEpoch();

    // Time the sync by the blocks it takes on, not by the progress percentage.
    // Percentage is dominated by the transaction weight of each block, so it
    // crawls through the middle of a sync and lurches near the end: an estimate
    // drawn from it says "unknown" for most of the wait, which is exactly when
    // the wait is worth estimating. Heights are integers and always advance.
    blockProcessTime.push_front(qMakePair(now, count));
    static const int MAX_SAMPLES = 5000;
    if (blockProcessTime.count() > MAX_SAMPLES) {
        blockProcessTime.remove(MAX_SAMPLES, blockProcessTime.count() - MAX_SAMPLES);
    }

    if (blockProcessTime.size() >= 2 && bestHeaderHeight > count) {
        // Average over a window long enough that one fast or slow batch does not
        // swing the answer, falling back to the whole (shorter) history while
        // the sync has only just started — so a figure appears within seconds.
        static const qint64 WINDOW_MSECS = 60 * 1000;
        qint64 remainingMSecs = -1;
        for (int i = 1; i < blockProcessTime.size(); i++) {
            const QPair<qint64, int>& sample = blockProcessTime[i];
            if (sample.first < now - WINDOW_MSECS || i == blockProcessTime.size() - 1) {
                const int blocks_done = count - sample.second;
                const qint64 elapsed = now - sample.first;
                if (blocks_done > 0 && elapsed > 0) {
                    remainingMSecs = (qint64)((double)(bestHeaderHeight - count) * (double)elapsed / (double)blocks_done);
                }
                break;
            }
        }
        if (remainingMSecs >= 0) {
            ui->expectedTimeLeft->setText(GUIUtil::formatNiceTimeOffset(remainingMSecs / 1000.0));
        } else {
            // No block taken on within the window yet: say so rather than guess.
            ui->expectedTimeLeft->setText(QObject::tr("unknown"));
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

void ModalOverlay::FitInfoText()
{
    // Give each warning paragraph a floor equal to the height its own text
    // occupies at the width it was actually given. Without this the paragraphs
    // render clipped: the words wrap, but the space allotted is one line short.
    // Measure only once the label holds its real share of the panel. Called
    // while the layout is still settling it reports a width of ~100px, where the
    // text of course needs a great deal of height — and because these are rich
    // text, the document keeps that stale layout, so every later measurement
    // repeats the wrong answer and the panel stays stretched.
    const int panel = ui->contentWidget->width();
    for (QLabel* l : {ui->infoText, ui->infoTextStrong}) {
        if (l->isHidden()) continue;
        const int w = l->width();
        if (w <= 0 || (panel > 0 && w * 2 < panel)) continue;
        const int needed = l->heightForWidth(w);
        if (needed > 0 && needed != l->minimumHeight()) l->setMinimumHeight(needed);
    }
}

void ModalOverlay::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    // The widths are only real once the panel has been laid out, so measure on
    // the next turn of the event loop rather than now.
    QTimer::singleShot(0, this, [this] { FitInfoText(); });
}

void ModalOverlay::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    FitInfoText();
}
