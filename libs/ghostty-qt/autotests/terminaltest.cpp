// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Terminal.h"
#include "Frame.h"
#include "Unicode.h"

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QColor>
#include <QEvent>
#include <QIODevice>
#include <QImage>
#include <QKeyEvent>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRgb>
#include <QSignalSpy>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTest>
#include <QVector>

using namespace GhosttyQt;

namespace
{
// Qt swaps Control and Command on macOS by default.
#ifdef Q_OS_MACOS
constexpr auto terminalControlModifier = Qt::MetaModifier;
#else
constexpr auto terminalControlModifier = Qt::ControlModifier;
#endif

bool gesture(Terminal &terminal, SelectionEvent::Type type, QPoint cell, bool rectangle = false)
{
    static quint64 timestamp{1};

    return terminal.selectionGesture({
        .type = type,
        .cell = cell,
        .position = QPointF(cell.x() * 10 + (type == SelectionEvent::Type::Drag ? 9 : 5), cell.y() * 20 + 10),
        .timestamp = timestamp++,
        .repeatInterval = 500,
        .repeatDistance = 5,
        .rectangle = rectangle,
    });
}

bool clickSelection(Terminal &terminal, QPoint cell, int clicks)
{
    terminal.cancelSelectionGesture();

    for (int click = 0; click < clicks; ++click) {
        if (click && !gesture(terminal, SelectionEvent::Type::Release, cell))
            return false;

        if (!gesture(terminal, SelectionEvent::Type::Press, cell))
            return false;
    }

    return true;
}
}

class TerminalTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    // Completed OSC 7 sequences update directory metadata, which clears on an empty report or terminal reset.
    void workingDirectoryEffects()
    {
        Terminal terminal;
        QStringList directories;
        connect(&terminal, &Terminal::workingDirectoryChanged, this, [&] {
            directories.append(terminal.workingDirectoryUrl());
        });

        QVERIFY(terminal.feed("plain output\x1b]7;file:///tmp"));
        QVERIFY(directories.isEmpty());
        QVERIFY(terminal.feed("\x1b\\more output"));
        QCOMPARE(directories, QStringList{QStringLiteral("file:///tmp")});
        QVERIFY(terminal.feed("\r\nordinary output"));
        QCOMPARE(directories.size(), 1);

        QVERIFY(terminal.feed("\x1b]7;file://remote.example/home/user\x1b\\\x1b]7;\x1b\\"));
        QCOMPARE(directories.at(1), QStringLiteral("file://remote.example/home/user"));
        QVERIFY(directories.last().isEmpty());

        QVERIFY(
            terminal.feed("\x1b]7;file:///tmp\x1b\\\x1b"
                          "c"));
        QVERIFY(directories.last().isEmpty());
        QVERIFY(terminal.workingDirectoryUrl().isEmpty());
        QVERIFY(terminal.feed("\x1b]7;file:///tmp\x1b\\"));
        terminal.reset();
        QVERIFY(directories.last().isEmpty());
        QVERIFY(terminal.workingDirectoryUrl().isEmpty());
    }

    // Color scheme replies follow the configured background through application color overrides and resets.
    void colorSchemeQueriesFollowConfiguredBackground()
    {
        Terminal terminal;
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        terminal.setColors(Qt::white, Qt::black, {});
        QVERIFY(terminal.feed("\x1b[?996n"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;1n"));

        terminal.setColors(Qt::black, Qt::white, {});
        QVERIFY(terminal.feed("\x1b[?996n"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;2n"));

        QVERIFY(terminal.feed("\x1b]11;#000000\x1b\\\x1b[?996n"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;2n"));
        QVERIFY(terminal.feed("\x1b]111\x1b\\\x1b[?996n"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;2n"));
    }

    // Color scheme changes produce replies only while reporting is enabled.
    void colorSchemeChangesReportedWhenRequested()
    {
        Terminal terminal;
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        terminal.setColors(Qt::white, Qt::black, {});
        QVERIFY(replies.isEmpty());
        QVERIFY(terminal.feed("\x1b[?2031h"));
        terminal.setColors(Qt::black, Qt::white, {});
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;2n"));
        terminal.setColors(Qt::white, Qt::black, {});
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?997;1n"));
        QVERIFY(terminal.feed("\x1b[?2031l"));
        terminal.setColors(Qt::black, Qt::white, {});
        QVERIFY(replies.isEmpty());
    }

    // The bold foreground override preserves explicit colors, inverse video and underline colors in exported cells.
    void boldDefaultForegroundRespectsCellStyles()
    {
        Terminal terminal{12, 2};
        const QColor foreground{12, 34, 56};
        const QColor background{3, 4, 5};
        const QColor bold{220, 210, 200};
        const QVector<QRgb> palette{qRgb(1, 2, 3), qRgb(150, 20, 30)};
        terminal.setColors(foreground, background, palette, bold);
        QVERIFY(
            terminal.feed("N\x1b[1mB\x1b[31mA\x1b[38;2;12;34;56mR\x1b[39;7mI\x1b[27;2mF"
                          "\x1b[22;1;4mU\x1b[58;2;70;80;90mC\x1b[0mN\x1b[1;48;2;40;50;60mE\x1b[7mV\x1b[22;27;49mN"));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const auto &row = frame.rows.at(0);
        QCOMPARE(row.at(0).foreground, foreground.rgb());
        QCOMPARE(row.at(1).foreground, bold.rgb());
        QVERIFY(row.at(1).flags & Bold);
        QCOMPARE(row.at(1).background, background.rgb());
        QVERIFY(row.at(1).defaultBackground);
        QCOMPARE(row.at(2).foreground, palette.at(1)); // Bold does not promote ANSI colors.
        QCOMPARE(row.at(3).foreground, foreground.rgb()); // Explicit RGB equal to the default is still explicit.
        QCOMPARE(row.at(4).foreground, background.rgb());
        QCOMPARE(row.at(4).background, bold.rgb());
        QVERIFY(!row.at(4).defaultBackground);
        QCOMPARE(row.at(5).foreground, bold.rgb());
        QVERIFY(row.at(5).flags & Faint);
        QCOMPARE(row.at(6).foreground, bold.rgb());
        QCOMPARE(row.at(6).underlineColor, bold.rgb());
        QCOMPARE(row.at(7).foreground, bold.rgb());
        QCOMPARE(row.at(7).underlineColor, qRgb(70, 80, 90));
        QCOMPARE(row.at(8).foreground, foreground.rgb());
        QVERIFY(!(row.at(8).flags & Bold));
        QCOMPARE(row.at(9).foreground, bold.rgb());
        QCOMPARE(row.at(9).background, qRgb(40, 50, 60));
        QVERIFY(!row.at(9).defaultBackground);
        QCOMPARE(row.at(10).foreground, qRgb(40, 50, 60));
        QCOMPARE(row.at(10).background, bold.rgb());
        QVERIFY(!row.at(10).defaultBackground);
        QCOMPARE(row.at(11).foreground, foreground.rgb());
        QCOMPARE(row.at(11).background, background.rgb());
        QVERIFY(row.at(11).defaultBackground);
        QVERIFY(!(row.at(11).flags & Bold));
    }

    // Terminal output produces replies, title and bell signals, styled Unicode cells and retained primary-screen text.
    void emulationAndEffects()
    {
        Terminal terminal{12, 4};
        QVERIFY(terminal.isValid());
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        QSignalSpy titles{&terminal, &Terminal::titleChanged};
        QSignalSpy bells{&terminal, &Terminal::bell};
        QVERIFY(terminal.feed("hello\x1b[6n\x1b]2;title\x07\x07"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.at(0).at(0).toByteArray(), QByteArray("\x1b[1;6R"));
        QCOMPARE(titles.size(), 1);
        QCOMPARE(titles.at(0).at(0).toString(), QStringLiteral("title"));
        QCOMPARE(bells.size(), 1);
        QVERIFY(terminal.feed("\x1b[2J\x1b[H\x1b[1;3;4;38;2;1;2;3mA\x1b[0m界é"));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.rows.size(), 4);
        QCOMPARE(frame.rows[0][0].foreground, qRgb(1, 2, 3));
        QCOMPARE(frame.rows[0][0].flags & (Bold | Italic | Underline), Bold | Italic | Underline);
        QCOMPARE(frame.rows[0][1].width, 2);
        QCOMPARE(frame.rows[0][2].width, 0);
        QCOMPARE(frame.rows[0][3].text, QStringLiteral("é"));
        QVERIFY(terminal.select({0, 0}, {3, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("A界é"));
        terminal.feed("\x1b[?1049hALT");
        QVERIFY(terminal.isModeEnabled(Mode::AlternateScreen));
        terminal.feed("\x1b[?1049l");
        QVERIFY(!terminal.isModeEnabled(Mode::AlternateScreen));
        QVERIFY(terminal.historyText().contains(QStringLiteral("A界é")));
    }

    // Application color overrides survive palette updates until their reset sequences restore the configured colors.
    void colorsAndDecorations()
    {
        Terminal terminal{12, 4};
        terminal.setColors(QColor(0xaa, 0xbb, 0xcc), QColor(0x11, 0x22, 0x33), {});
        terminal.setDecorations(QColor(0, 255, 0), Qt::white, Qt::blue);
        terminal.feed("\x1b]10;#123456\x1b\\\x1b]11;#654321\x1b\\\x1b]12;#102030\x1b\\");
        terminal.setColors(QColor(0xaa, 0xbb, 0xcc), QColor(0x11, 0x22, 0x33), {});
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.foreground, QColor(0x12, 0x34, 0x56));
        QCOMPARE(frame.background, QColor(0x65, 0x43, 0x21));
        QCOMPARE(frame.cursorColor, QColor(0x10, 0x20, 0x30));
        terminal.feed("\x1b]110\x1b\\\x1b]111\x1b\\\x1b]112\x1b\\\x1b[7;4:3;58:2::1:2:3mA");
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.foreground, QColor(0xaa, 0xbb, 0xcc));
        QCOMPARE(frame.background, QColor(0x11, 0x22, 0x33));
        QCOMPARE(frame.cursorColor, QColor(Qt::green));
        QCOMPARE(frame.rows[0][0].foreground, frame.background.rgb());
        QCOMPARE(frame.rows[0][0].background, frame.foreground.rgb());
        QCOMPARE(frame.rows[0][0].underline, 3);
        QCOMPARE(frame.rows[0][0].underlineColor, qRgb(1, 2, 3));
        QVERIFY(!frame.rows[0][0].defaultBackground);
    }

    // Both halves of a wide cell carry the same colors, decorations and style flags.
    void wideCellAppearance()
    {
        Terminal terminal{12, 4};
        terminal.feed("\x1b[1;3;7;4:3;38;2;1;2;3;48;2;4;5;6;58:2::7:8:9m界\x1b[0m");
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const Cell &lead = frame.rows[0][0];
        const Cell &tail = frame.rows[0][1];
        QCOMPARE(lead.width, 2);
        QCOMPARE(tail.width, 0);
        QCOMPARE(tail.foreground, lead.foreground);
        QCOMPARE(tail.background, lead.background);
        QCOMPARE(tail.underlineColor, lead.underlineColor);
        QCOMPARE(tail.underline, lead.underline);
        QCOMPARE(tail.flags, lead.flags);
        QCOMPARE(tail.defaultBackground, lead.defaultBackground);
    }

    // Word and line drags retain their anchors through scrolling and reflow, and reset clears the selection.
    void selectionGestures()
    {
        Terminal terminal{20, 5};
        QVERIFY(terminal.resize(20, 5, {10, 20}));
        terminal.feed("alpha beta gamma\r\nlogical line wraps over columns\r\nlast line");
        QVERIFY(clickSelection(terminal, {7, 0}, 2));
        QCOMPARE(terminal.selectionText(), QStringLiteral("beta"));
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {12, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("beta gamma"));
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {1, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("alpha beta"));
        QVERIFY(clickSelection(terminal, {6, 2}, 3));
        QCOMPARE(terminal.selectionText(), QStringLiteral("logical line wraps over columns"));
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {3, 3}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("logical line wraps over columns\nlast line"));
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {3, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("alpha beta gamma\nlogical line wraps over columns"));
        QVERIFY(clickSelection(terminal, {7, 0}, 2));
        terminal.feed("\r\nnew one\r\nnew two\r\nnew three");
        terminal.scrollTo(0);
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {12, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("beta gamma"));
        QVERIFY(terminal.resize(10, 5));
        terminal.scrollTo(0);
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {2, 1}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("beta gamma"));
        terminal.feed("\x1b[?1049h\x1b[Hword");
        QVERIFY(clickSelection(terminal, {0, 0}, 2));
        terminal.feed(
            "\x1b"
            "c");
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {0, 0}));
        QVERIFY(terminal.selectionText().isEmpty());
    }

    // Enabled terminal modes determine encoded key, focus, paste and mouse input and supported query replies.
    void inputAndQueries()
    {
        Terminal terminal;
        terminal.setTerminfoName("xterm-256color");
        terminal.feed("\x1b[?2004h\x1b[?1004h\x1b[?1000h\x1b[?1006h\x1b[?1h");
        QKeyEvent up{QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier};
        QCOMPARE(terminal.encodeKey(up), QByteArray("\x1bOA"));
        QKeyEvent controlC{QEvent::KeyPress, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03'))};
        QCOMPARE(terminal.encodeKey(controlC), QByteArray(1, '\x03'));
        QCOMPARE(terminal.encodeFocus(true), QByteArray("\x1b[I"));
        QCOMPARE(terminal.encodePaste("hello"), QByteArray("\x1b[200~hello\x1b[201~"));
        MouseEvent mouse;
        mouse.button = Qt::LeftButton;
        mouse.position = {25, 35};
        mouse.cellSize = {10, 20};
        mouse.size = {800, 480};
        QCOMPARE(terminal.encodeMouse(mouse), QByteArray("\x1b[<0;3;2M"));
        mouse.action = MouseEvent::Action::Release;
        QCOMPARE(terminal.encodeMouse(mouse), QByteArray("\x1b[<0;3;2m"));
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        terminal.feed("\x1b]11;?\x1b\\\x1bP$qm\x1b\\\x1bP+q544e\x1b\\");
        QByteArray output;

        for (const auto &args : replies)
            output += args[0].toByteArray();
        QVERIFY(output.contains("\x1b]11;rgb:"));
        QVERIFY(output.contains("\x1bP1$r"));
        QVERIFY(output.contains("\x1bP1+r544E="));
    }

    // Clipboard access is denied when no clipboard handler is installed.
    void clipboardNegotiation()
    {
        Terminal terminal;
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        terminal.feed("\x1b[?5522$p");
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?5522;0$y"));
        terminal.feed("\x1b[?5522h\x1b[?5522$p");
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b[?5522;0$y"));
        terminal.feed("\x1b]5522;type=read:id=test;dGV4dC9wbGFpbg==\x1b\\");
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b]5522;type=read:status=EPERM:id=test\x1b\\"));
        terminal.feed("\x1b]5522;type=write\x1b\\\x1b]5522;type=wdata\x1b\\");
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().at(0).toByteArray(), QByteArray("\x1b]5522;type=write:status=EPERM\x1b\\"));
        terminal.feed("\x1b]52;c;?\x07");
        QVERIFY(replies.isEmpty());
    }

    // Qt key events encode modifiers, application keypad keys and Kitty press, repeat and release events.
    void qtKeyTranslation()
    {
        Terminal terminal;
        QKeyEvent backtab{QEvent::KeyPress, Qt::Key_Backtab, Qt::NoModifier, QStringLiteral("\t")};
        QCOMPARE(terminal.encodeKey(backtab), QByteArray("\x1b[Z"));
        QKeyEvent shifted{QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")};
        QCOMPARE(terminal.encodeKey(shifted), QByteArray("A"));
        QKeyEvent alt{QEvent::KeyPress, Qt::Key_A, Qt::AltModifier, QStringLiteral("a")};
        QCOMPARE(terminal.encodeKey(alt, {}, true),
                 QByteArray("\x1b"
                            "a"));
        QKeyEvent altgr{QEvent::KeyPress, Qt::Key_At, terminalControlModifier | Qt::AltModifier | Qt::GroupSwitchModifier, QStringLiteral("@")};
        QCOMPARE(terminal.encodeKey(altgr), QByteArray("@"));
        QKeyEvent function{QEvent::KeyPress, Qt::Key_F5, terminalControlModifier};
        QCOMPARE(terminal.encodeKey(function), QByteArray("\x1b[15;5~"));
        terminal.feed("\x1b=\x1b[?1035l");
        QKeyEvent keypad{QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier, QStringLiteral("1")};
        QCOMPARE(terminal.encodeKey(keypad), QByteArray("\x1bOq"));
        terminal.feed("\x1b[>3u");
        QKeyEvent press{QEvent::KeyPress, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03'))};
        QCOMPARE(terminal.encodeKey(press), QByteArray("\x1b[99;5u"));
        QKeyEvent repeat{QEvent::KeyPress, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03')), true};
        QCOMPARE(terminal.encodeKey(repeat), QByteArray("\x1b[99;5:2u"));
        QKeyEvent release{QEvent::KeyRelease, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03'))};
        QCOMPARE(terminal.encodeKey(release), QByteArray("\x1b[99;5:3u"));
    }

    // Caps Lock and Num Lock affect keypad output and Kitty modifier bits according to the active modes.
    void keyboardLocks()
    {
        Terminal terminal;
        QKeyEvent letter{QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("A")};
        QCOMPARE(terminal.encodeKey(letter, {.capsLock = true}), QByteArray("A"));
        terminal.feed("\x1b=\x1b[?1035l");
        QKeyEvent keypad{QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier, QStringLiteral("1")};
        QCOMPARE(terminal.encodeKey(keypad), QByteArray("\x1bOq"));
        QCOMPARE(terminal.encodeKey(keypad, {.numLock = true}), QByteArray("\x1bOq"));
        terminal.feed("\x1b[?1035h");
        QCOMPARE(terminal.encodeKey(keypad, {.numLock = true}), QByteArray("1"));
        terminal.feed("\x1b[>3u");
        QKeyEvent control{QEvent::KeyPress, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03'))};
        QCOMPARE(terminal.encodeKey(control, {.capsLock = true}), QByteArray("\x1b[99;69u"));
        QCOMPARE(terminal.encodeKey(control, {.numLock = true}), QByteArray("\x1b[99;133u"));
        QCOMPARE(terminal.encodeKey(control, {.capsLock = true, .numLock = true}), QByteArray("\x1b[99;197u"));
        QKeyEvent release{QEvent::KeyRelease, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03'))};
        QCOMPARE(terminal.encodeKey(release, {.capsLock = true}), QByteArray("\x1b[99;69:3u"));
        QKeyEvent repeat{QEvent::KeyPress, Qt::Key_C, terminalControlModifier, QString(QChar(u'\x03')), true};
        QCOMPARE(terminal.encodeKey(repeat, {.numLock = true}), QByteArray("\x1b[99;133:2u"));
    }

    // Mouse motion and wheel events encode their buttons, modifiers and terminal cell coordinates.
    void qtMouseTranslation()
    {
        Terminal terminal;
        terminal.feed("\x1b[?1003h\x1b[?1006h");
        MouseEvent mouse;
        mouse.action = MouseEvent::Action::Move;
        mouse.button = Qt::RightButton;
        mouse.buttons = Qt::RightButton;
        mouse.modifiers = terminalControlModifier;
        mouse.position = {25, 35};
        mouse.cellSize = {10, 20};
        mouse.size = {800, 480};
        QCOMPARE(terminal.encodeMouse(mouse), QByteArray("\x1b[<50;3;2M"));
        mouse.action = MouseEvent::Action::Press;
        mouse.button = Qt::NoButton;
        mouse.buttons = Qt::NoButton;
        mouse.modifiers = Qt::NoModifier;
        mouse.wheel = MouseEvent::WheelDirection::Up;
        QCOMPARE(terminal.encodeMouse(mouse), QByteArray("\x1b[<64;3;2M"));
        mouse.wheel = MouseEvent::WheelDirection::Right;
        QCOMPARE(terminal.encodeMouse(mouse), QByteArray("\x1b[<67;3;2M"));
    }

    // Terminal metadata exposes links and working directories and consumes the latest sanitized notification once.
    void passiveMetadata()
    {
        Terminal terminal{20, 3};
        terminal.feed("\x1b]8;id=test;https://example.org/path\x1b\\link\x1b]8;;\x1b\\ plain");
        QCOMPARE(terminal.linkAt({2, 0}), QStringLiteral("https://example.org/path"));
        QVERIFY(terminal.linkAt({6, 0}).isEmpty());
        QVERIFY(terminal.linkAt({-1, 0}).isEmpty());
        terminal.feed("\x1b]7;file://localhost/tmp/project%20name\x07");
        QCOMPARE(terminal.workingDirectoryUrl(), QStringLiteral("file://localhost/tmp/project%20name"));
        terminal.feed("\x1b]777;notify;Build ready;All checks passed\x07");
        const auto notification = terminal.takeNotification();
        QVERIFY(notification);
        QCOMPARE(notification->title, QStringLiteral("Build ready"));
        QCOMPARE(notification->body, QStringLiteral("All checks passed"));
        QVERIFY(!terminal.takeNotification());
        terminal.feed("\x1b]9;first\x07\x1b]9;last\xe2\x80\xae\x07");
        const auto latest = terminal.takeNotification();
        QVERIFY(latest);
        QCOMPARE(latest->body, QStringLiteral("last"));
    }

    void progressReports_data()
    {
        QTest::addColumn<QByteArray>("reports");
        QTest::addColumn<Progress::State>("state");
        QTest::addColumn<int>("percent");

        const QByteArray known{"\x1b]9;4;1;42\x07"};
        QTest::newRow("zero") << QByteArray("\x1b]9;4;1;0\x07") << Progress::State::Normal << 0;
        QTest::newRow("complete") << QByteArray("\x1b]9;4;1;100\x07") << Progress::State::Normal << 100;
        QTest::newRow("over-100") << QByteArray("\x1b]9;4;1;999\x07") << Progress::State::Normal << 100;
        QTest::newRow("pause-value") << known + "\x1b]9;4;4;0\x07" << Progress::State::Paused << 0;
        QTest::newRow("pause-retains-value") << known + "\x1b]9;4;4\x07" << Progress::State::Paused << 42;
        QTest::newRow("pause-without-value") << QByteArray("\x1b]9;4;4\x07") << Progress::State::Paused << -1;
        QTest::newRow("error-value") << known + "\x1b]9;4;2;73\x07" << Progress::State::Error << 73;
        QTest::newRow("error-without-value") << known + "\x1b]9;4;2\x07" << Progress::State::Error << -1;
        QTest::newRow("remove-ignores-value") << known + "\x1b]9;4;0;90\x07" << Progress::State::None << -1;
    }

    // Progress reports clamp percentages and retain the previous percentage when a pause omits it.
    void progressReports()
    {
        QFETCH(QByteArray, reports);
        QFETCH(Progress::State, state);
        QFETCH(int, percent);

        Terminal terminal;
        QVERIFY(terminal.feed(reports));
        QCOMPARE(terminal.progress().state, state);
        QCOMPARE(terminal.progress().percent, percent);
    }

    // Terminal resets clear progress state and prevent later pauses from recovering the old percentage.
    void progressReset()
    {
        Terminal terminal;

        for (bool inputReset : {false, true}) {
            QVERIFY(terminal.feed("\x1b]9;4;1;42\x07"));

            if (inputReset) {
                QVERIFY(
                    terminal.feed("\x1b"
                                  "c"));
            } else {
                terminal.reset();
            }

            QCOMPARE(terminal.progress().state, Progress::State::None);
            QCOMPARE(terminal.progress().percent, -1);
            QVERIFY(terminal.feed("\x1b]9;4;4\x07"));
            QCOMPARE(terminal.progress().state, Progress::State::Paused);
            QCOMPARE(terminal.progress().percent, -1);
        }
    }

    // Snapshots retain replaced image data, and file or shared-memory transfers are rejected.
    void imageSnapshotsAndTransferRestrictions()
    {
        Terminal terminal{20, 4};
        terminal.resize(20, 4, {10, 20});
        terminal.feed("\x1b_Ga=T,i=1,s=1,v=1,f=32,c=2,r=1;/wAA/w==\x1b\\");
        Frame before;
        QVERIFY(terminal.snapshot(before));
        QCOMPARE(before.images.size(), 1);
        QCOMPARE(before.images.at(0).image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(before.images.at(0).pixelSize, QSize(20, 20));
        QSignalSpy replies{&terminal, &Terminal::writeRequested};

        for (const char medium : {'f', 't', 's'}) {
            const QByteArray request = QByteArray("\x1b_Ga=q,i=32,s=1,v=1,f=24,t=") + medium + ";L3RtcC9rd2ktbm90LWFuLWltYWdl\x1b\\";
            terminal.feed(request);
            QVERIFY(!replies.isEmpty());
            const auto response = replies.takeLast().at(0).toByteArray();
            QVERIFY2(response.contains("unsupported medium"), response.constData());
        }

        QImage png{2, 2, QImage::Format_RGBA8888};
        png.fill(Qt::green);
        QByteArray bytes;
        QBuffer buffer{&bytes};
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(png.save(&buffer, "PNG"));
        const QByteArray imageCommand{QByteArrayLiteral("\x1b_Ga=T,i=2,f=100;") + bytes.toBase64() + "\x1b\\"};
        QVERIFY(terminal.feed(imageCommand));
        Frame after;
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(after.images.size(), 2);
        QCOMPARE(after.images.at(1).image.pixelColor(0, 0), QColor(Qt::green));
        terminal.feed("\x1b_Ga=T,i=1,s=1,v=1,f=32,c=2,r=1;AAD//w==\x1b\\");
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(before.images.at(0).image.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(after.images.at(0).image.pixelColor(0, 0), QColor(Qt::blue));
        const quint64 primaryGeneration = after.images.at(0).generation;
        terminal.feed("\x1b[?1049h\x1b_Ga=T,i=1,s=1,v=1,f=32,c=1,r=1;/wAA/w==\x1b\\");
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(after.images.size(), 1);
        QCOMPARE(after.images.at(0).image.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(after.images.at(0).generation != primaryGeneration);
        terminal.feed("\x1b[?1049l");
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(after.images.at(0).image.pixelColor(0, 0), QColor(Qt::blue));
    }

    // Prompt markers support command output and history navigation, and clearing scrollback preserves modes until reset.
    void promptsAndHistory()
    {
        Terminal terminal{20, 3};

        for (int i = 0; i < 5; ++i) {
            terminal.feed(
                "\x1b]133;A\x07$ \x1b]133;B\x07"
                "command\r\n\x1b]133;C\x07result\r\n\x1b]133;D;0\x07");
        }

        terminal.feed("\x1b]133;A\x07$ \x1b]133;B\x07");
        QCOMPARE(terminal.commandOutput(), QStringLiteral("result"));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const auto bottom = frame.scrollOffset;
        terminal.scrollPrompt(-1);
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.scrollOffset < bottom);
        terminal.scrollPrompt(100);
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, bottom);
        terminal.feed("\x1b[?2004h");
        terminal.clearScrollback();
        QVERIFY(terminal.isModeEnabled(Mode::BracketedPaste));
        terminal.reset();
        QVERIFY(!terminal.isModeEnabled(Mode::BracketedPaste));
        QVERIFY(terminal.historyText().isEmpty());
    }

    // Reading the latest command output preserves the selection and remains independent of cursor position.
    void recentCommandOutput()
    {
        Terminal terminal{80, 4};
        terminal.feed(
            "\x1b]133;A\x07$ \x1b]133;B\x07first\r\n\x1b]133;C\x07older\r\n\x1b]133;D;0\x07"
            "\x1b]133;A\x07$ \x1b]133;B\x07second\r\n\x1b]133;C\x07latest\x1b]133;D;0\x07"
            "\x1b]133;A\x07$ \x1b]133;B\x07");
        QVERIFY(terminal.select({0, 0}, {0, 0}));
        const QString selected = terminal.selectionText();
        QCOMPARE(terminal.commandOutput(), QStringLiteral("latest"));
        QCOMPARE(terminal.selectionText(), selected);
        terminal.feed("\x1b[H");
        QCOMPARE(terminal.commandOutput(), QStringLiteral("latest"));
        terminal.reset();
        QVERIFY(terminal.commandOutput().isEmpty());
    }

    // Fragmented input preserves Unicode styling, and resize or reset keeps the selected scrollback policy.
    void fragmentedInputAndUnlimitedHistory()
    {
        Terminal terminal{80, 5, Terminal::UnlimitedScrollback};
        const QByteArray input = "\x1b[38;2;42;43;44m界\x1b[0m";

        for (char byte : input)
            QVERIFY(terminal.feed(QByteArrayView(&byte, 1)));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.rows[0][0].foreground, qRgb(42, 43, 44));
        QCOMPARE(frame.rows[0][0].width, 2);
        terminal.reset();

        for (int i = 0; i < 2000; ++i)
            terminal.feed(QByteArray("history-" + QByteArray::number(i) + "\r\n"));
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollTotal, 2001);
        terminal.feed("\x1b[?2004h\x1b[?1049hALTERNATE");
        terminal.setScrollbackLines(512);
        QVERIFY(terminal.historyText().contains(QStringLiteral("ALTERNATE")));
        terminal.feed("\x1b[?1049l");
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.scrollTotal < 2001);
        QVERIFY(terminal.historyText().contains(QStringLiteral("history-1999")));
        QVERIFY(!terminal.historyText().contains(QStringLiteral("history-0\n")));
        QVERIFY(terminal.isModeEnabled(Mode::BracketedPaste));
        terminal.setScrollbackLines(0);
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollTotal, 5);
        terminal.setScrollbackLines(Terminal::UnlimitedScrollback);
        terminal.feed("unlimited-again\r\n");

        for (int i = 0; i < 2000; ++i)
            terminal.feed("new history\r\n");
        QVERIFY(terminal.resize(40, 10));
        QVERIFY(terminal.historyText().contains(QStringLiteral("unlimited-again\n")));
        terminal.feed(
            "\x1b"
            "creset-unlimited\r\n");

        for (int i = 0; i < 2000; ++i)
            terminal.feed("after reset\r\n");
        QVERIFY(terminal.historyText().contains(QStringLiteral("reset-unlimited\n")));
    }

    void clearScrollbackPreservesScreenAndModes_data()
    {
        QTest::addColumn<bool>("alternate");
        QTest::newRow("primary") << false;
        QTest::newRow("alternate") << true;
    }

    // Clearing scrollback preserves screen cells, the cursor and terminal modes on either screen.
    void clearScrollbackPreservesScreenAndModes()
    {
        QFETCH(bool, alternate);
        Terminal terminal{20, 4};
        const QByteArray output{QByteArray("old history\r\n") + QByteArray("filler\r\n").repeated(100) + "primary screen"};
        QVERIFY(terminal.feed(output));

        if (alternate) {
            QVERIFY(terminal.feed("\x1b[?1049hALT SCREEN"));
        }

        QVERIFY(terminal.feed("\x1b[?2004h"));
        Frame before;
        QVERIFY(terminal.snapshot(before));
        terminal.clearScrollback();
        Frame after;
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(after.rows, before.rows);
        QCOMPARE(after.cursor, before.cursor);
        QVERIFY(terminal.isModeEnabled(Mode::BracketedPaste));
        QCOMPARE(terminal.isModeEnabled(Mode::AlternateScreen), alternate);

        if (alternate) {
            QVERIFY(terminal.feed("\x1b[?1049l"));
        }

        QVERIFY(!terminal.historyText().contains(QStringLiteral("old history")));
        QVERIFY(terminal.historyText().contains(QStringLiteral("primary screen")));
        QVERIFY(terminal.snapshot(after));
        QCOMPARE(after.scrollTotal, 4U);
    }

    // Rectangular selections mark the expected cells, and drag anchors follow content as new rows arrive.
    void selectionAnchorsAndRectangles()
    {
        Terminal terminal{12, 3};
        QVERIFY(terminal.resize(12, 3, {10, 20}));
        terminal.feed("012345\r\nabcdef\r\nUVWXYZ");
        QVERIFY(terminal.select({3, 2}, {1, 0}, true));
        QCOMPARE(terminal.selectionText(), QStringLiteral("123\nbcd\nVWX"));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.rows[0][1].flags & Selected);
        QVERIFY(!(frame.rows[0][0].flags & Selected));
        terminal.clearSelection();
        QVERIFY(terminal.selectionText().isEmpty());
        QVERIFY(gesture(terminal, SelectionEvent::Type::Press, {0, 1}));
        terminal.feed("\r\nnew row");
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {2, 1}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("abcdef\nUVW"));
        terminal.clearSelection();
        QVERIFY(gesture(terminal, SelectionEvent::Type::Drag, {0, 0}));
        QVERIFY(terminal.selectionText().isEmpty());
        QVERIFY(terminal.selectAll());
        QCOMPARE(terminal.selectionText(), terminal.historyText());
    }

    // Selection adjustments move or extend the selected text and return the resulting cell position.
    void semanticSelectionAdjustment()
    {
        Terminal terminal{8, 3};
        QVERIFY(terminal.feed("abcde\r\nfghij\r\nklmno"));
        QVERIFY(terminal.select({2, 1}, {2, 1}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("h"));

        auto cursor = terminal.adjustSelection(SelectionAdjustment::Right, true);
        QVERIFY(cursor);
        QCOMPARE(*cursor, QPoint(3, 1));
        QCOMPARE(terminal.selectionText(), QStringLiteral("hi"));

        cursor = terminal.adjustSelection(SelectionAdjustment::Up, false);
        QVERIFY(cursor);
        QCOMPARE(*cursor, QPoint(3, 0));
        QCOMPARE(terminal.selectionText(), QStringLiteral("d"));

        cursor = terminal.adjustSelection(SelectionAdjustment::StartOfLine, true);
        QVERIFY(cursor);
        QCOMPARE(*cursor, QPoint(0, 0));
        QCOMPARE(terminal.selectionText(), QStringLiteral("abcd"));

        terminal.clearSelection();
        QVERIFY(!terminal.adjustSelection(SelectionAdjustment::Right, false));
    }

    // Reset preserves grapheme cell widths, and history extraction handles wide and combining text at the line edge.
    void unicodeWidthAndReset()
    {
        Terminal terminal{20, 3};
        terminal.feed("👩‍💻🇺🇳❤️");
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.cursor.x(), 6);
        QCOMPARE(frame.rows[0][0].text, QStringLiteral("👩‍💻"));
        QCOMPARE(frame.rows[0][0].width, 2);
        terminal.feed(
            "\x1b[?2027l\x1b"
            "c👩‍💻");
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.cursor.x(), 2);

        for (bool wrap : {true, false}) {
            Terminal narrow{9, 3};

            if (!wrap)
                narrow.feed("\x1b[?7l");
            narrow.feed("Älpha 界é");
            QCOMPARE(narrow.historyText(), QStringLiteral("Älpha 界é"));
        }
    }

    // Frames preserve cursor and palette overrides, and grapheme queries report UTF-16 lengths and cell widths.
    void cursorAndPalettePolicy()
    {
        Terminal terminal{12, 3};
        terminal.setDefaultCursor(CursorStyle::Underline, false);
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.cursorStyle, CursorStyle::Underline);
        QVERIFY(!frame.cursorBlinking);
        terminal.feed("\x1b[5 q界\x1b[1;2H");
        terminal.setDefaultCursor(CursorStyle::Block, false);
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.cursorStyle, CursorStyle::Beam);
        QVERIFY(frame.cursorBlinking);
        QVERIFY(frame.cursorWideTail);
        terminal.feed("\x1b[0 q\x1b[?25l");
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.cursorStyle, CursorStyle::Block);
        QVERIFY(!frame.cursorVisible);
        terminal.feed("\x1b[?25h\x1b[H\x1b]4;1;#123456\x1b\\\x1b[31mA\x1b[1mB\x1b[91mC");
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.rows[0][0].foreground, qRgb(0x12, 0x34, 0x56));
        QCOMPARE(frame.rows[0][1].foreground, frame.rows[0][0].foreground);
        QVERIFY(frame.rows[0][1].flags & Bold);
        QVERIFY(frame.rows[0][2].foreground != frame.rows[0][1].foreground);
        terminal.setDecorations(Qt::yellow, Qt::black, Qt::cyan);
        terminal.feed("\x1b]17;#ff0000\x1b\\\x1b]19;#ffffff\x1b\\");
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.selectionBackground, QColor(Qt::cyan));
        QCOMPARE(frame.selectionForeground, QColor(Qt::black));
        const auto graphemes = GhosttyQt::graphemes(QStringLiteral("é界👩‍💻"));
        QCOMPARE(graphemes.size(), 3);
        QCOMPARE(graphemes[0].length, 2);
        QCOMPARE(graphemes[0].width, 1);
        QCOMPARE(graphemes[1].length, 1);
        QCOMPARE(graphemes[1].width, 2);
        QCOMPARE(graphemes[2].length, 5);
        QCOMPARE(graphemes[2].width, 2);
    }

    // Invalid images are rejected without interrupting text output or exposing clipboard data.
    void rejectedImagesAndClipboard()
    {
        Terminal terminal{20, 3};
        QSignalSpy replies{&terminal, &Terminal::writeRequested};
        QVERIFY(
            terminal.feed("\x1b_Ga=T,i=4,s=4294967295,v=4294967295,f=32;AAAA\x1b\\"
                          "\x1b_Ga=T,i=5,f=100;bm90cG5n\x1b\\"
                          "\x1b]52;c;ZXh0ZXJuYWwgY2xpcGJvYXJk\x07"
                          "\x1b]52;c;?\x07"
                          "safe"));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.images.isEmpty());
        QCOMPARE(frame.rows[0][0].text, QStringLiteral("s"));

        for (const auto &reply : replies)
            QVERIFY(!reply[0].toByteArray().contains("ZXh0ZXJuYWwgY2xpcGJvYXJk"));
    }
};

QTEST_GUILESS_MAIN(TerminalTest)
#include "terminaltest.moc"
