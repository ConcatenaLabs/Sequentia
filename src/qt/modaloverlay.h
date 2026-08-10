// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MODALOVERLAY_H
#define BITCOIN_QT_MODALOVERLAY_H

#include <qt/guiutil.h>

#include <QDateTime>
#include <QPropertyAnimation>
#include <QWidget>

//! The required delta of headers to the estimated number of available headers until we show the IBD progress
static constexpr int HEADER_HEIGHT_DELTA_SYNC = 24;

namespace Ui {
    class ModalOverlay;
}

/** Modal overlay to display information about the chain-sync state */
class ModalOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit ModalOverlay(bool enable_wallet, QWidget *parent);
    ~ModalOverlay();

    void tipUpdate(int count, const QDateTime& blockDate, double nVerificationProgress);
    void setKnownBestHeight(int count, const QDateTime& blockDate);

    // will show or hide the modal layer
    void showHide(bool hide = false, bool userRequested = false);
    bool isLayerVisible() const { return layerIsVisible; }

public Q_SLOTS:
    void toggleVisibility();
    void closeClicked();

Q_SIGNALS:
    void triggered(bool hidden);

protected:
    void showEvent(QShowEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;

    bool eventFilter(QObject * obj, QEvent * ev) override;
    bool event(QEvent* ev) override;

private:
    //! Give the wrapping warning paragraphs the height their text needs at the
    //! width they were given, so the last line is not cut off.
    void FitInfoText();

    Ui::ModalOverlay *ui;
    int bestHeaderHeight; //best known height (based on the headers)
    /** SEQUENTIA: header height when this session's header sync started, so the
        header percentage runs 0→100% over the headers we actually have to
        fetch instead of counting from genesis. -1 until the first update. */
    int m_header_sync_start_height = -1;
    /** SEQUENTIA: measures the chain's real block spacing so "headers left" is
        not derived from the consensus constant (600s here vs ~34s actual). */
    GUIUtil::HeaderSyncEstimator m_header_estimator;
    QDateTime bestHeaderDate;
    //! (wall-clock ms, block height) samples, newest first: the sync rate is
    //! measured in blocks, which always advance, rather than in the progress
    //! percentage, which can sit still for most of a sync.
    QVector<QPair<qint64, int> > blockProcessTime;
    bool layerIsVisible;
    bool userClosed;
    QPropertyAnimation m_animation;
    void UpdateHeaderSyncLabel();
    /** Headers still missing, from the measured chain spacing. */
    int EstimatedHeadersLeft() const;
};

#endif // BITCOIN_QT_MODALOVERLAY_H
