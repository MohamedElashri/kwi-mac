// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "QtInput_p.h"
#include "Terminal.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTest>

class TerminalInputTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void modifierTransitionsDoNotWrite()
    {
        GhosttyQt::Terminal terminal;
        for (auto key : {Qt::Key_Shift, Qt::Key_Control, Qt::Key_Meta, Qt::Key_Alt}) {
            QKeyEvent press{QEvent::KeyPress, key, Qt::ShiftModifier | Qt::MetaModifier};
            QKeyEvent release{QEvent::KeyRelease, key, Qt::NoModifier};
            QVERIFY2(terminal.encodeKey(press).isEmpty(), "Modifier-only press must not encode a printable/control byte");
            QVERIFY(terminal.encodeKey(release).isEmpty());
        }
    }

    void nativeLayoutPositions()
    {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
            // A physical ANSI A key can produce Q on a non-US layout.
            QKeyEvent event{QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, 0, 0, 0, QStringLiteral("q")};
            QCOMPARE(GhosttyQt::keyInput(event).key, GHOSTTY_KEY_A);
            QCOMPARE(GhosttyQt::keyInput(event).text, QByteArray("q"));
            QKeyEvent control{QEvent::KeyRelease, Qt::Key_Meta, Qt::NoModifier, 0, 0x3b, 0};
            QCOMPARE(GhosttyQt::keyInput(control).key, GHOSTTY_KEY_CONTROL_LEFT);
        }
#endif
    }

    void synthesizedFunctionKeys()
    {
        GhosttyQt::Terminal terminal;
        QKeyEvent up{QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier};
        QCOMPARE(terminal.encodeKey(up), QByteArray("\033[A"));
        QKeyEvent backtab{QEvent::KeyPress, Qt::Key_Backtab, Qt::NoModifier, QStringLiteral("\t")};
        QCOMPARE(terminal.encodeKey(backtab), QByteArray("\033[Z"));
        QKeyEvent function{QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier};
        QCOMPARE(terminal.encodeKey(function), QByteArray("\033[15~"));
    }

    void controlAndCommand()
    {
        using namespace GhosttyQt;
#ifdef Q_OS_MACOS
        const auto physicalControl = Qt::MetaModifier;
        const auto command = Qt::ControlModifier;
#else
        const auto physicalControl = Qt::ControlModifier;
        const auto command = Qt::MetaModifier;
#endif
        Terminal terminal;
        for (const auto key : {Qt::Key_C, Qt::Key_D, Qt::Key_Z}) {
            const QString text{QChar(key - Qt::Key_A + 1)};
            QKeyEvent event{QEvent::KeyPress, key, physicalControl, text};
            QCOMPARE(keyInput(event).modifiers, GhosttyMods(GHOSTTY_MODS_CTRL));
            QCOMPARE(terminal.encodeKey(event), text.toUtf8());
        }

        QKeyEvent copy{QEvent::KeyPress, Qt::Key_C, command, QStringLiteral("c")};
        QCOMPARE(keyInput(copy).modifiers, GhosttyMods(GHOSTTY_MODS_SUPER));
        QVERIFY(terminal.encodeKey(copy) != QByteArray(1, '\x03'));
        QCOMPARE(ghosttyModifiers(physicalControl | Qt::ShiftModifier), GhosttyMods(GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT));
    }

    void embeddingHostAttribute()
    {
#ifdef Q_OS_MACOS
        QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, true);
        const auto control = GhosttyQt::ghosttyModifiers(Qt::ControlModifier);
        const auto command = GhosttyQt::ghosttyModifiers(Qt::MetaModifier);
        QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, false);
        QCOMPARE(control, GhosttyMods(GHOSTTY_MODS_CTRL));
        QCOMPARE(command, GhosttyMods(GHOSTTY_MODS_SUPER));
#endif
    }

    void optionAsAlt()
    {
#ifdef Q_OS_MACOS
        GhosttyQt::Terminal terminal;
        // Native Cocoa logical key follows the layout, not the physical position.
        QKeyEvent composed{QEvent::KeyPress, Qt::Key_V, Qt::AltModifier, 0, 9, 1UL << 19, QString::fromUtf8("√")};
        QCOMPARE(terminal.encodeKey(composed), QString::fromUtf8("√").toUtf8());
        QCOMPARE(terminal.encodeKey(composed, {}, true), QByteArray("\x1bv"));
        QKeyEvent dead{QEvent::KeyPress, Qt::Key_E, Qt::AltModifier, 0, 14, 1UL << 19, QString { }};
        QCOMPARE(terminal.encodeKey(dead, {}, true), QByteArray("\x1b") + "e");
        QKeyEvent layout{QEvent::KeyPress, Qt::Key_Q, Qt::AltModifier, 0, 0, 1UL << 19, QString::fromUtf8("œ")};
        QCOMPARE(terminal.encodeKey(layout, {}, true), QByteArray("\x1bq"));
        QKeyEvent shifted{QEvent::KeyPress, Qt::Key_V, Qt::AltModifier | Qt::ShiftModifier, 0, 9, (1UL << 19) | (1UL << 17), QString::fromUtf8("◊")};
        QCOMPARE(terminal.encodeKey(shifted, {}, true), QByteArray("\x1bV"));
        QKeyEvent
            capsShift{QEvent::KeyPress, Qt::Key_V, Qt::AltModifier | Qt::ShiftModifier, 0, 9, (1UL << 19) | (1UL << 17) | (1UL << 16), QStringLiteral("v")};
        QCOMPARE(terminal.encodeKey(capsShift, {.capsLock = true}, true), QByteArray("\033v"));
        QKeyEvent arrow{QEvent::KeyPress, Qt::Key_Left, Qt::AltModifier, 0, 0x7b, 1UL << 19};
        QVERIFY(terminal.encodeKey(arrow, {}, true).contains("1;3D"));
#endif
    }

    void textAndProtocolEvents()
    {
        using namespace GhosttyQt;
        Terminal terminal;
        QKeyEvent text{QEvent::KeyPress, Qt::Key_E, Qt::AltModifier, QString::fromUtf8("é")};
#ifdef Q_OS_MACOS
        QCOMPARE(terminal.encodeKey(text), QString::fromUtf8("é").toUtf8());
#endif
        QKeyEvent release{QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a")};
        QCOMPARE(keyInput(release).action, GHOSTTY_KEY_ACTION_RELEASE);
        QCOMPARE(terminal.encodeKey(release), QByteArray{});
        terminal.feed(QByteArrayView{"\x1b[>3u"}); // Kitty disambiguation + event types.
        QVERIFY(!terminal.encodeKey(release).isEmpty());
        QKeyEvent repeat{QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"), true};
        QCOMPARE(keyInput(repeat).action, GHOSTTY_KEY_ACTION_REPEAT);
        QCOMPARE(keyInput(repeat).consumedModifiers, GhosttyMods(GHOSTTY_MODS_SHIFT));
    }
};

QTEST_MAIN(TerminalInputTest)
#include "TerminalInputTest.moc"
