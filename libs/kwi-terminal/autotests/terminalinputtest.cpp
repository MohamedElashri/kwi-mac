// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProcessTestSupport.h"
#include "Session.h"
#include "TerminalView.h"

#include <KLocalizedString>
#include <KShell>
#include <KUrlMimeData>
#include <QByteArray>
#include <QChar>
#include <QClipboard>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QIODevice>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLatin1Char>
#include <QList>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSignalSpy>
#include <QSizeF>
#include <QString>
#include <QStringLiteral>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWheelEvent>
#include <QtQuickTest/quicktest.h>

using namespace Kwi;

namespace
{
class InputFixture
{
public:
    InputFixture()
        : view(window.contentItem())
    {
        inputClock.start();
        window.resize(640, 400);
        view.setSize(QSizeF(640, 400));
        view.setSession(&session);
#ifdef Q_OS_MACOS
        view.setOptionAsAlt(true);
#endif
        origin = view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().topLeft();
        window.show();
        view.forceActiveFocus();
    }

    bool start()
    {
        if (!directory.isValid() || !QTest::qWaitForWindowExposed(&window) || !QQuickTest::qWaitForPolish(&view)) {
            return false;
        }

        if (!session.start(testLaunch(QStringLiteral(KWI_TEST_PYTHON),
                                      {QStringLiteral("-u"),
                                       QStringLiteral("-c"),
                                       QStringLiteral("import os,sys,tty\n"
                                                      "tty.setraw(0)\n"
                                                      "capture=open(sys.argv[1],'wb',buffering=0)\n"
                                                      "os.write(1,b'READY')\n"
                                                      "while True:\n"
                                                      " data=os.read(0,4096)\n"
                                                      " if not data: break\n"
                                                      " capture.write(data)\n"),
                                       directory.filePath(QStringLiteral("input"))},
                                      directory.path()))) {
            return false;
        }

        return QTest::qWaitFor(
            [this] {
                return session.historyText().contains(QStringLiteral("READY"));
            },
            5000);
    }

    QByteArray captured() const
    {
        QFile file{directory.filePath(QStringLiteral("input"))};

        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }

        return file.readAll();
    }

    QUrl hoveredLinkAt(QPointF point)
    {
        QHoverEvent event{QEvent::HoverMove, point, point, point, Qt::ControlModifier};
        QCoreApplication::sendEvent(&view, &event);

        return view.hoveredLink();
    }

    void key(QEvent::Type type, int key, Qt::KeyboardModifiers modifiers, const QString &text = {})
    {
        // These are logical synthetic keys, with no Cocoa hardware keycode.
        QKeyEvent event{type, key, modifiers, 0, 0xffffffffu, 0, text};
        QCoreApplication::sendEvent(&view, &event);
    }

    void mouse(QMouseEvent &event)
    {
        event.setTimestamp(quint64(inputClock.elapsed()));
        QCoreApplication::sendEvent(&view, &event);
    }

    QPointF position(qreal column = 0.5, qreal row = 0.5) const
    {
        const QSizeF cell{view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().size()};

        return origin + QPointF(column * cell.width(), row * cell.height());
    }

    void wheel(QPoint pixels, QPoint angles, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QWheelEvent event{position(), view.mapToGlobal(position()), pixels, angles, Qt::NoButton, modifiers, Qt::NoScrollPhase, false};
        QCoreApplication::sendEvent(&view, &event);
    }

    QPointF origin;
    QElapsedTimer inputClock;
    Session session;
    QTemporaryDir directory;
    QQuickWindow window;
    TerminalView view;
};

}

class TerminalInputTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void controlAndUnicodeKeys();
    void tabStaysInTerminal();
    void applicationCursorAndKeypad();
    void contextMenuDoesNotSendInput();
    void confirmsAndSanitizesPaste();
    void reportsMouseAndHover();
    void reportsMouseTrackingModes_data();
    void reportsMouseTrackingModes();
    void alternateScrollHonorsMode();
    void readOnlyBlocksWheelInput();
    void linkHoverAndActivation();
    void dropsQuotedPathsAndTextWithoutExecuting_data();
    void dropsQuotedPathsAndTextWithoutExecuting();
    void confirmsMultilineDropsAndRejectsMoveOnly();
    void rectangularSelectionOverridesMouseReporting();
    void readOnlyBlocksInputButRepliesToTerminalQueries();
    void copyOnSelectIsOptIn();
    void keyboardSelectionDoesNotSendInput();
};

// Control, Alt, AltGr and Unicode key events deliver the expected bytes to the child.
void TerminalInputTest::controlAndUnicodeKeys()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.key(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
    fixture.key(QEvent::KeyRelease, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
    fixture.key(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier, QString(QChar(0)));
    fixture.key(QEvent::KeyRelease, Qt::Key_Space, Qt::ControlModifier, QString(QChar(0)));
    fixture.key(QEvent::KeyPress, Qt::Key_X, Qt::AltModifier, QStringLiteral("x"));
    fixture.key(QEvent::KeyRelease, Qt::Key_X, Qt::AltModifier, QStringLiteral("x"));
    fixture.key(QEvent::KeyPress, Qt::Key_At, Qt::ControlModifier | Qt::AltModifier | Qt::GroupSwitchModifier, QStringLiteral("@"));
    fixture.key(QEvent::KeyRelease, Qt::Key_At, Qt::ControlModifier | Qt::AltModifier | Qt::GroupSwitchModifier, QStringLiteral("@"));
    fixture.key(QEvent::KeyPress, 0x00e9, Qt::NoModifier, QStringLiteral("é"));
    fixture.key(QEvent::KeyRelease, 0x00e9, Qt::NoModifier, QStringLiteral("é"));
    QTRY_COMPARE(fixture.captured(), QByteArray::fromHex("03001b7840c3a9"));
}

// Tab and Shift+Tab reach the child while terminal focus stays in place.
void TerminalInputTest::tabStaysInTerminal()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    QQuickItem next{fixture.window.contentItem()};
    next.setActiveFocusOnTab(true);
    fixture.view.forceActiveFocus();
    fixture.key(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    fixture.key(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    fixture.key(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier, QStringLiteral("\t"));
    fixture.key(QEvent::KeyRelease, Qt::Key_Backtab, Qt::ShiftModifier, QStringLiteral("\t"));
    QTRY_COMPARE(fixture.captured(), QByteArray("\t\x1b[Z"));
    QVERIFY(fixture.view.hasActiveFocus());
}

// Cursor and keypad keys use their application-mode escape sequences.
void TerminalInputTest::applicationCursorAndKeypad()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[?1h\x1b[?1035l\x1b=");
    fixture.key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    fixture.key(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    fixture.key(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier, QStringLiteral("\r"));
    fixture.key(QEvent::KeyRelease, Qt::Key_Enter, Qt::KeypadModifier, QStringLiteral("\r"));
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1bOA\x1bOM"));
}

// Menu and Shift+F10 request a context menu without sending terminal key reports.
void TerminalInputTest::contextMenuDoesNotSendInput()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[>3u");
    QSignalSpy menus{&fixture.view, &TerminalView::contextMenuRequested};
    fixture.key(QEvent::KeyPress, Qt::Key_Menu, Qt::NoModifier);
    fixture.key(QEvent::KeyRelease, Qt::Key_Menu, Qt::NoModifier);
    fixture.key(QEvent::KeyPress, Qt::Key_F10, Qt::ShiftModifier);
    fixture.key(QEvent::KeyRelease, Qt::Key_F10, Qt::ShiftModifier);
    QCOMPARE(menus.size(), 2);
    fixture.key(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
    fixture.key(QEvent::KeyRelease, Qt::Key_C, Qt::ControlModifier, QString(QChar(3)));
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[99;5u\x1b[99;5:3u"));
}

// Multiline clipboard paste requests confirmation and removes unsafe controls before bracketed delivery.
void TerminalInputTest::confirmsAndSanitizesPaste()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[?2004h");
    const QString paste = QStringLiteral("one\r\ntwo\x1b[201~\x03\x7f\t") + QChar(0x85);
    QGuiApplication::clipboard()->setText(paste);
    QSignalSpy requested{&fixture.view, &TerminalView::multilinePasteRequested};
    fixture.view.paste();
    QCOMPARE(requested.size(), 1);
    QVERIFY(fixture.captured().isEmpty());
    QCOMPARE(requested.first().at(0).value<Kwi::Session *>(), &fixture.session);
    fixture.session.pasteApprovedText(requested.first().at(1).toString());
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[200~one\rtwo[201~\t\x1b[201~"));
}

// Mouse presses, releases and hover movement include cell coordinates and modifiers in terminal reports.
void TerminalInputTest::reportsMouseAndHover()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    QTest::mouseMove(&fixture.window, fixture.position(1.5, 0.5).toPoint());
    fixture.session.feed("\x1b[?1003h\x1b[?1006h");
    QMouseEvent press{QEvent::MouseButtonPress, fixture.position(), fixture.position(), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier};
    QCoreApplication::sendEvent(&fixture.view, &press);
    QMouseEvent release{QEvent::MouseButtonRelease, fixture.position(), fixture.position(), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier};
    QCoreApplication::sendEvent(&fixture.view, &release);
    QTest::mouseMove(&fixture.window, fixture.position().toPoint());
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[<16;1;1M\x1b[<16;1;1m\x1b[<35;1;1M"));
}

void TerminalInputTest::reportsMouseTrackingModes_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("press-only") << 9 << QByteArray("\x1b[<0;1;1M");
    QTest::newRow("press-release") << 1000 << QByteArray("\x1b[<0;1;1M\x1b[<0;2;1m");
    QTest::newRow("button-motion") << 1002 << QByteArray("\x1b[<0;1;1M\x1b[<32;2;1M\x1b[<0;2;1m");
    QTest::newRow("any-motion") << 1003 << QByteArray("\x1b[<0;1;1M\x1b[<32;2;1M\x1b[<0;2;1m\x1b[<35;3;1M");
}

// Mouse tracking modes determine which press, release and movement events reach the child.
void TerminalInputTest::reportsMouseTrackingModes()
{
    QFETCH(int, mode);
    QFETCH(QByteArray, expected);
    InputFixture fixture;
    QVERIFY(fixture.start());
    const qreal cellWidth = fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().width();
    const QPointF start{fixture.position()};
    const QPointF dragged = start + QPointF(cellWidth, 0);
    const QPointF hovered = dragged + QPointF(cellWidth, 0);
    QTest::mouseMove(&fixture.window, dragged.toPoint());
    fixture.session.feed("\x1b[?" + QByteArray::number(mode) + "h\x1b[?1006h");
    QMouseEvent press{QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &press);
    QMouseEvent move{QEvent::MouseMove, dragged, dragged, Qt::NoButton, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &move);
    QMouseEvent release{QEvent::MouseButtonRelease, dragged, dragged, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &release);
    QTest::mouseMove(&fixture.window, hovered.toPoint());
    fixture.session.send("!");
    QTRY_VERIFY(fixture.captured().endsWith('!'));
    QCOMPARE(fixture.captured(), expected + '!');
}

// Wheel events produce cursor keys on the alternate screen only when alternate scrolling is enabled.
void TerminalInputTest::alternateScrollHonorsMode()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[?1049h\x1b[?1007l");
    QWheelEvent disabled{fixture.position(), fixture.position(), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false};
    QCoreApplication::sendEvent(&fixture.view, &disabled);
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));
    fixture.session.feed("\x1b[?1007h");
    QWheelEvent enabled{fixture.position(), fixture.position(), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false};
    QCoreApplication::sendEvent(&fixture.view, &enabled);
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n") + QByteArray("\x1b[A").repeated(QGuiApplication::styleHints()->wheelScrollLines()));
}

// Read-only sessions suppress wheel reports and alternate-screen cursor keys while still answering terminal queries.
void TerminalInputTest::readOnlyBlocksWheelInput()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.setReadOnly(true);
    fixture.session.feed("\x1b[?1000h\x1b[?1006h");
    fixture.wheel({0, 120}, {0, 360});
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));

    fixture.session.feed("\x1b[?1000l\x1b[?1049h\x1b[?1007h");
    fixture.wheel({0, 120}, {0, 360});
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n\x1b[0n"));
}

// Explicit and detected links support hover lookup, and Ctrl-click emits the selected URL.
void TerminalInputTest::linkHoverAndActivation()
{
    InputFixture fixture;
    fixture.session.feed("\x1b]8;;https://example.org/explicit\x07label\x1b]8;;\x07\r\nhttps://example.org/a_(b).");
    QTRY_COMPARE(fixture.hoveredLinkAt(fixture.position()), QUrl(QStringLiteral("https://example.org/explicit")));
    QCOMPARE(fixture.hoveredLinkAt(fixture.position(0.5, 1.5)), QUrl(QStringLiteral("https://example.org/a_(b)")));
    QHoverEvent hover{QEvent::HoverMove, fixture.position(), fixture.position(), fixture.position(1.5, 0.5), Qt::ControlModifier};
    QCoreApplication::sendEvent(&fixture.view, &hover);
    QCOMPARE(fixture.view.hoveredLink(), QUrl(QStringLiteral("https://example.org/explicit")));
    QSignalSpy activated{&fixture.view, &TerminalView::linkActivated};
    QMouseEvent press{QEvent::MouseButtonPress, fixture.position(), fixture.position(), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier};
    QCoreApplication::sendEvent(&fixture.view, &press);
    QMouseEvent release{QEvent::MouseButtonRelease, fixture.position(), fixture.position(), Qt::LeftButton, Qt::NoButton, Qt::ControlModifier};
    QCoreApplication::sendEvent(&fixture.view, &release);
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().first().toUrl(), QUrl(QStringLiteral("https://example.org/explicit")));
    QHoverEvent leave{QEvent::HoverLeave, fixture.position(), fixture.position(), fixture.position()};
    QCoreApplication::sendEvent(&fixture.view, &leave);
    QVERIFY(fixture.view.hoveredLink().isEmpty());
    QVERIFY(!fixture.session.isActive());
}

void TerminalInputTest::dropsQuotedPathsAndTextWithoutExecuting_data()
{
    QTest::addColumn<int>("mimeFormat");
    QTest::newRow("standard-uri-list") << 0;
    QTest::newRow("kde-prefer-local") << 1;
    QTest::newRow("kde-only") << 2;
}

// Dropped paths and URLs are quoted, and plain text is inserted without submitting a command.
void TerminalInputTest::dropsQuotedPathsAndTextWithoutExecuting()
{
    QFETCH(int, mimeFormat);
    InputFixture fixture;
    QVERIFY(fixture.start());
    const QString path = QStringLiteral("/tmp/a b'$(touch nope).txt");
    const QUrl remote{QStringLiteral("https://example.org/a?x=1&y=2")};
    QMimeData files;
    const QList<QUrl> urls{QUrl::fromLocalFile(path), remote};

    if (mimeFormat == 0) {
        files.setUrls(urls);
    } else if (mimeFormat == 1) {
        KUrlMimeData::setUrls({QUrl(QStringLiteral("desktop:/different")), remote}, urls, &files);
    } else {
        KUrlMimeData::setUrls(urls, {}, &files);
        files.removeFormat(QStringLiteral("text/uri-list"));
        files.removeFormat(QStringLiteral("text/plain"));
        QVERIFY(!files.hasUrls());
        QVERIFY(!files.hasText());
    }

    QDragEnterEvent enter{fixture.position().toPoint(), Qt::CopyAction | Qt::MoveAction, &files, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(enter.dropAction(), Qt::CopyAction);
    QDropEvent drop{fixture.position(), Qt::CopyAction | Qt::MoveAction, &files, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    const QByteArray expected = (KShell::quoteArg(path) + QLatin1Char(' ') + KShell::quoteArg(remote.toString(QUrl::FullyEncoded)) + QLatin1Char(' ')).toUtf8();
    QTRY_COMPARE(fixture.captured(), expected);
    QVERIFY(!fixture.captured().contains('\r'));
    QVERIFY(!fixture.captured().contains('\n'));

    QMimeData text;
    text.setText(QStringLiteral("echo 日本語"));
    QDropEvent textDrop{fixture.position(), Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &textDrop);
    QVERIFY(textDrop.isAccepted());
    const QByteArray withText = expected + QStringLiteral("echo 日本語").toUtf8();
    QTRY_COMPARE(fixture.captured(), withText);
}

// Multiline text drops request confirmation, and drops offering only a move action are rejected.
void TerminalInputTest::confirmsMultilineDropsAndRejectsMoveOnly()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    QMimeData text;
    text.setText(QStringLiteral("first\nsecond"));
    QSignalSpy requested{&fixture.view, &TerminalView::multilinePasteRequested};
    QDropEvent drop{fixture.position(), Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(1).toString(), text.text());
    QVERIFY(fixture.captured().isEmpty());
    QDropEvent moveOnly{fixture.position(), Qt::MoveAction, &text, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &moveOnly);
    QVERIFY(!moveOnly.isAccepted());
    QCOMPARE(requested.size(), 1);
    QVERIFY(fixture.captured().isEmpty());
}

// Rectangular selection keeps its initial mode throughout a drag and suppresses terminal mouse reports.
void TerminalInputTest::rectangularSelectionOverridesMouseReporting()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[2J\x1b[Halpha beta\r\nbravo char\r\ncider date\x1b[?1000h\x1b[?1006h");
    QTRY_VERIFY(fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().y()
                >= 2 * fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().height());
    const QPointF start{fixture.position(1.5, 0.5)};
    const QPointF end{fixture.position(3.9, 2.5)};
    QMouseEvent press{QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier};
    fixture.mouse(press);
    // Releasing modifiers during the drag must preserve the initial selection mode.
    QMouseEvent move{QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier};
    fixture.mouse(move);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("lph\nrav\nide"));
    QMouseEvent release{QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
    fixture.mouse(release);
    QCOMPARE(fixture.session.selectionText(), QStringLiteral("lph\nrav\nide"));
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));
}

// Read-only mode blocks user input while preserving copying and terminal protocol replies.
void TerminalInputTest::readOnlyBlocksInputButRepliesToTerminalQueries()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[?1000h\x1b[?1006h");
    fixture.session.setReadOnly(true);
    fixture.key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    fixture.view.requestPaste(QStringLiteral("paste"));
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("日本語"));
    QCoreApplication::sendEvent(&fixture.view, &commit);
    QMouseEvent press{QEvent::MouseButtonPress, fixture.position(), fixture.position(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &press);
    QMimeData mime;
    mime.setText(QStringLiteral("drop"));
    QDropEvent drop{fixture.position(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(&fixture.view, &drop);
    QVERIFY(!drop.isAccepted());
    QVERIFY(!fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    QVERIFY(fixture.captured().isEmpty());
    fixture.view.selectAll();
    fixture.view.copy();
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("READY")));
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));
    fixture.session.setReadOnly(false);
    fixture.key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0nx"));
}

// Selecting text updates the clipboard only when copy-on-select is enabled.
void TerminalInputTest::copyOnSelectIsOptIn()
{
    InputFixture fixture;
    fixture.session.feed("alpha beta\r\n");
    QTRY_VERIFY(fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().y() >= fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().height());
    auto select = [&](int start, int end) {
        const QPointF first{fixture.position(start + 0.5, 0.5)};
        const QPointF last{fixture.position(end + 0.9, 0.5)};
        QMouseEvent press{QEvent::MouseButtonPress, first, first, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
        fixture.mouse(press);
        QMouseEvent move{QEvent::MouseMove, last, last, Qt::NoButton, Qt::LeftButton, Qt::NoModifier};
        fixture.mouse(move);
        QMouseEvent release{QEvent::MouseButtonRelease, last, last, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
        fixture.mouse(release);
    };
    QGuiApplication::clipboard()->setText(QStringLiteral("original"));
    select(0, 4);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("alpha"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("original"));
    QVERIFY(fixture.view.setProperty("copyOnSelect", true));
    select(6, 9);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("beta"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
}

// Keyboard selection handles wide cells and copying without sending keys or IME commits to the child.
void TerminalInputTest::keyboardSelectionDoesNotSendInput()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    fixture.session.feed("\x1b[2J\x1b[Halpha beta");
    QTRY_VERIFY(fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().left()
                > fixture.origin.x() + 9 * fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().width());
    fixture.view.setKeyboardSelectionMode(true);
    QVERIFY(fixture.view.keyboardSelectionMode());
    QVERIFY(!fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("日本語"));
    QCoreApplication::sendEvent(&fixture.view, &commit);
    fixture.key(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("a"));
    fixture.key(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    fixture.key(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("alp"));
    fixture.key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QVERIFY(!fixture.view.keyboardSelectionMode());
    QVERIFY(fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("alp"));
    QVERIFY(fixture.captured().isEmpty());
    fixture.session.feed(QStringLiteral("\x1b[2J\x1b[HA界B").toUtf8());
    QTRY_VERIFY(fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().left()
                < fixture.origin.x() + 5 * fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().width());
    fixture.view.setKeyboardSelectionMode(true);
    fixture.key(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    fixture.key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("界"));
    QCOMPARE(fixture.view.selectionCursorRectangle().width(), 2 * fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().width());
    fixture.key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("B"));
    fixture.key(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QVERIFY(!fixture.view.keyboardSelectionMode());
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));
}

int main(int argc, char **argv)
{
    QTemporaryDir configuration;

    if (!configuration.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", QFile::encodeName(configuration.path()));
#ifdef Q_OS_MACOS
    // Exercise physical Ctrl semantics independently of Cocoa's Command swap.
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
#endif
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("kwi");
    TerminalInputTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "terminalinputtest.moc"
