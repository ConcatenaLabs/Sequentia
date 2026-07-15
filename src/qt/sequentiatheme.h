// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SEQUENTIATHEME_H
#define BITCOIN_QT_SEQUENTIATHEME_H

#include <QColor>

class QApplication;

/**
 * Sequentia Core dark theme.
 *
 * Apply() installs a dark QPalette and the accompanying stylesheet
 * (:/css/sequentia) on the whole application. Call it once at startup, right
 * after the QApplication exists and before any window is created, so every
 * widget — including native dialogs and message boxes — inherits the theme.
 *
 * The palette and the stylesheet must agree on the base colours; the canonical
 * values live here as named constants and are mirrored in res/css/sequentia.css.
 */
namespace SequentiaTheme {

// Canonical palette (keep in sync with res/css/sequentia.css).
inline QColor Bg()        { return QColor(0x0b, 0x0b, 0x0d); }
inline QColor Panel()     { return QColor(0x14, 0x14, 0x17); }
inline QColor Panel2()    { return QColor(0x19, 0x19, 0x20); }
inline QColor Line()      { return QColor(0x26, 0x26, 0x2c); }
inline QColor Text()      { return QColor(0xf2, 0xf0, 0xea); }
inline QColor Muted()     { return QColor(0x9b, 0x98, 0x8e); }
inline QColor Faint()     { return QColor(0x6d, 0x6a, 0x62); }
inline QColor Accent()    { return QColor(0xf5, 0xb3, 0x01); }
inline QColor AccentInk() { return QColor(0x1a, 0x14, 0x00); }
inline QColor Good()      { return QColor(0x3e, 0xcf, 0x7a); }
inline QColor Warn()      { return QColor(0xff, 0xb8, 0x4d); }
inline QColor Bad()       { return QColor(0xff, 0x6b, 0x6b); }

/** Install the dark palette and stylesheet on the application. */
void Apply(QApplication& app);

} // namespace SequentiaTheme

#endif // BITCOIN_QT_SEQUENTIATHEME_H
