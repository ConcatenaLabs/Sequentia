// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/collapsiblesection.h>

#include <QEvent>
#include <QFontMetrics>
#include <QGroupBox>
#include <QLayout>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

CollapsibleSection::CollapsibleSection(QGroupBox* box, const QString& settings_key, bool open_by_default)
    : QObject(box), m_box(box), m_key(settings_key)
{
    if (!m_box) return;
    m_title = m_box->title();

    QLayout* inner = m_box->layout();
    if (inner) {
        // Move the card's whole layout into one content widget so a single
        // setVisible() folds the card. QWidget::setLayout steals a layout from
        // its previous parent widget and reparents every widget in it, which is
        // what makes adopting a finished card safe: the existing child pointers
        // the page holds keep pointing at the same widgets.
        m_content = new QWidget(m_box);
        m_content->setLayout(inner);

        QVBoxLayout* outer = new QVBoxLayout(m_box);
        // The group box's own frame supplies the padding; a second set of
        // margins here would indent the contents twice over.
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(m_content);
    }

    // The title strip is the click target, so the box must not swallow the
    // press somewhere else first.
    m_box->installEventFilter(this);
    m_box->setCursor(Qt::PointingHandCursor);

    QSettings settings;
    const bool open = settings.value(QStringLiteral("CollapsibleSection/") + m_key, open_by_default).toBool();
    setOpen(open);
}

CollapsibleSection* CollapsibleSection::adopt(QGroupBox* box, const QString& settings_key, bool open_by_default)
{
    return new CollapsibleSection(box, settings_key, open_by_default);
}

void CollapsibleSection::setOpen(bool open)
{
    m_open = open;
    if (m_content) m_content->setVisible(open);
    if (m_box) {
        // The theme keys the card's frame and padding off this property, so a
        // folded card leaves no empty body box behind its title. A dynamic
        // property only takes effect once the style re-reads it.
        m_box->setProperty("collapsed", !open);
        m_box->style()->unpolish(m_box);
        m_box->style()->polish(m_box);
    }
    updateTitle();
    if (!m_key.isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("CollapsibleSection/") + m_key, open);
    }
}

void CollapsibleSection::setSummary(const QString& text)
{
    if (m_summary == text) return;
    m_summary = text;
    updateTitle();
}

void CollapsibleSection::updateTitle()
{
    if (!m_box) return;
    // A plain text arrow rather than a style icon: QGroupBox draws its title as
    // text, so the indicator has to live in the string to sit beside it.
    const QString arrow = m_open ? QStringLiteral("▾  ") : QStringLiteral("▸  ");
    QString title = arrow + m_title;
    if (!m_open && !m_summary.isEmpty()) {
        title += QStringLiteral("   —   ") + m_summary;
    }
    m_box->setTitle(title);
    m_box->setToolTip(m_open ? tr("Click the title to fold this section away")
                             : tr("Click to open this section"));
}

int CollapsibleSection::titleStripHeight() const
{
    if (!m_box) return 0;
    // The title sits in the top frame of the group box; give the strip the
    // title's own height plus the frame's top margin so the whole visible row
    // is clickable rather than just the glyphs.
    const int text_h = m_box->fontMetrics().height();
    const int margin = m_box->style()->pixelMetric(QStyle::PM_LayoutTopMargin, nullptr, m_box);
    return text_h + margin;
}

bool CollapsibleSection::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_box) return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint pos = me->pos();
            // Closed, the card IS the title, so anywhere in it toggles. Open,
            // only the title strip does -- otherwise a click that missed a
            // field by a few pixels would fold the form the user was filling in.
            const bool on_title = pos.y() <= titleStripHeight();
            if (!m_open || on_title) {
                toggle();
                return true;
            }
        }
    }
    return QObject::eventFilter(watched, event);
}
