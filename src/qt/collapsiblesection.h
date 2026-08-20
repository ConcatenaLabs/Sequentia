// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_COLLAPSIBLESECTION_H
#define BITCOIN_QT_COLLAPSIBLESECTION_H

#include <QObject>
#include <QString>

class QGroupBox;
class QLabel;
class QWidget;

/**
 * Turns an already-built QGroupBox into a card that folds down to its title.
 *
 * The Assets and Staking pages each stack half a dozen full forms on one
 * scrolling page, so the thing you came for is somewhere below three things you
 * did not. Folding a card to its title row turns each page back into a menu you
 * can read at a glance, and the one card you are actually using is the only one
 * open.
 *
 * Adoption rather than a new container: the pages build their cards with plain
 * QGroupBox and add widgets straight to them, so this takes the finished box,
 * moves its layout into an inner content widget, and shows or hides that one
 * widget. Nothing about how a card is built has to change, and every existing
 * pointer into it stays valid.
 *
 * Open/closed is remembered per card in QSettings under the given key, so the
 * page comes back the way it was left.
 */
class CollapsibleSection : public QObject
{
    Q_OBJECT

public:
    /**
     * @param box            the finished card; must already have a layout and a title
     * @param settings_key   stable id for remembering this card's state
     * @param open_by_default whether the card is open the first time it is seen
     */
    CollapsibleSection(QGroupBox* box, const QString& settings_key, bool open_by_default);

    bool isOpen() const { return m_open; }
    void setOpen(bool open);
    void toggle() { setOpen(!m_open); }

    /**
     * One line shown, dimmed, beside the title while the card is closed: the
     * answer the card would have given had it been open ("Staked: 40,000 tSEQ").
     * A closed card that still tells you where you stand is worth more than one
     * that has to be opened to find out nothing changed.
     */
    void setSummary(const QString& text);

    //! Convenience: adopt @p box and return the section (parented to the box).
    static CollapsibleSection* adopt(QGroupBox* box, const QString& settings_key,
                                     bool open_by_default = false);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateTitle();
    //! Height of the clickable title strip at the top of the card.
    int titleStripHeight() const;

    QGroupBox* m_box{nullptr};
    QWidget* m_content{nullptr};
    QString m_key;
    QString m_title;
    QString m_summary;
    bool m_open{false};
};

#endif // BITCOIN_QT_COLLAPSIBLESECTION_H
