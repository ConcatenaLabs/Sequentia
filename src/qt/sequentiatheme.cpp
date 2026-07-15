// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sequentiatheme.h>

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

namespace SequentiaTheme {

void Apply(QApplication& app)
{
    // Fusion is the only built-in style that honours a custom palette
    // consistently across platforms; the native Windows style ignores most of
    // it, which is why the app used to look pale there.
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window,          Bg());
    pal.setColor(QPalette::WindowText,      Text());
    pal.setColor(QPalette::Base,            Panel2());
    pal.setColor(QPalette::AlternateBase,   Panel());
    pal.setColor(QPalette::ToolTipBase,     QColor(0x1e, 0x1e, 0x26));
    pal.setColor(QPalette::ToolTipText,     Text());
    pal.setColor(QPalette::Text,            Text());
    pal.setColor(QPalette::Button,          Panel2());
    pal.setColor(QPalette::ButtonText,      Text());
    pal.setColor(QPalette::BrightText,      Bad());
    pal.setColor(QPalette::Link,            Accent());
    pal.setColor(QPalette::Highlight,       Accent());
    pal.setColor(QPalette::HighlightedText, AccentInk());
    pal.setColor(QPalette::PlaceholderText, Faint());

    // Disabled roles: dim the text so inactive controls read as inactive.
    pal.setColor(QPalette::Disabled, QPalette::WindowText, Faint());
    pal.setColor(QPalette::Disabled, QPalette::Text,       Faint());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, Faint());
    pal.setColor(QPalette::Disabled, QPalette::Highlight,  Line());
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, Muted());

    app.setPalette(pal);

    // Refinements a palette cannot express (borders, paddings, the card look).
    QFile css(":/css/sequentia");
    if (css.open(QFile::ReadOnly | QFile::Text)) {
        app.setStyleSheet(QString::fromUtf8(css.readAll()));
    }
}

} // namespace SequentiaTheme
