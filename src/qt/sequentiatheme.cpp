// Copyright (c) 2026 The Sequentia developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sequentiatheme.h>

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace SequentiaTheme {

void Apply(QApplication& app)
{
    // Fusion is the only built-in style that honours a custom palette
    // consistently across platforms; the native Windows style ignores most of
    // it, which is why the app used to look pale there.
    app.setStyle(QStyleFactory::create("Fusion"));

    // Bump the base reading size ~15% above the platform default. The default
    // (typically 9pt on Windows) is cramped for a wallet a non-technical user
    // reads all day. Scaling the existing font rather than pinning an absolute
    // size keeps it proportional to the machine's DPI and locale defaults, and
    // every widget inherits it because this runs before any window is created.
    // Point sizes bumped elsewhere relative to the current font (page titles,
    // the balance headline) scale along with it, staying in proportion.
    {
        QFont f = app.font();
        const qreal kFontScale = 1.15;
        if (f.pointSizeF() > 0) {
            f.setPointSizeF(f.pointSizeF() * kFontScale);
        } else if (f.pixelSize() > 0) {
            f.setPixelSize(qRound(f.pixelSize() * kFontScale));
        }
        app.setFont(f);
    }

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
