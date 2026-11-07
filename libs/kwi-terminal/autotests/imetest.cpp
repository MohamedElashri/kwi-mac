// SPDX-FileCopyrightText: 2026 Eike Hein <sho@eikehein.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"
#include "TerminalView.h"
#ifdef Q_OS_MACOS
#include "MacGuiTestSupport.h"
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QInputMethodEvent>
#include <QList>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QtQuickTest/quicktest.h>

namespace
{
class InputFixture
{
public:
    InputFixture()
        : view(window.contentItem())
    {
        window.resize(640, 400);
        view.setSize(QSizeF(window.size()));
        view.setSession(&session);
        window.show();
        window.requestActivate();
        view.forceActiveFocus();
    }

    bool start()
    {
        session.setExecutable(QStringLiteral(KWI_TEST_PYTHON));
        session.setArguments({QStringLiteral("-u"),
                              QStringLiteral("-c"),
                              QStringLiteral("import os,sys,tty\n"
                                             "tty.setraw(0)\n"
                                             "capture=open(sys.argv[1],'wb',buffering=0)\n"
                                             "os.write(1,('READY:'+str(os.getpid())).encode())\n"
                                             "while True:\n"
                                             " data=os.read(0,4096)\n"
                                             " if not data or data==b'\\x04': break\n"
                                             " capture.write(data)\n"),
                              directory.filePath(QStringLiteral("input"))});

        return directory.isValid() && session.start() && waitForReady();
    }

    bool waitForReady()
    {
        // Craft's macOS Python launcher can keep a parent process alive, so
        // the interpreter PID need not equal the PTY process ID. Require a
        // fresh startup marker for every launch, including a session restart.
        const int expectedReadyCount = readyCount + 1;
        const bool ready = QTest::qWaitFor(
            [this, expectedReadyCount] {
                return session.state() == Kwi::Session::State::Running && session.historyText().count(QStringLiteral("READY:")) == expectedReadyCount;
            },
            5000);
        if (!ready) {
            qWarning() << "IME fixture did not become ready" << session.state() << session.processId() << session.errorString() << session.historyText();
            return false;
        }
        readyCount = expectedReadyCount;
#ifdef Q_OS_MACOS
        return activateMacTestWindow(&window) && QQuickTest::qWaitForPolish(&view);
#else
        return QTest::qWaitForWindowActive(&window) && QQuickTest::qWaitForPolish(&view);
#endif
    }

    QByteArray captured() const
    {
        QFile file{directory.filePath(QStringLiteral("input"))};

        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }

    bool requestExit()
    {
        // Exit through the PTY so this also works across PID namespaces.
        return session.sendInput(QStringLiteral("\x04"));
    }

    QRectF caret() const
    {
        return view.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    }

    void compose(const QString &text, const QList<QInputMethodEvent::Attribute> &attributes = {}, const QString &commit = {})
    {
        QInputMethodEvent event(text, attributes);
        event.setCommitString(commit);
        QCoreApplication::sendEvent(&view, &event);
    }

    int readyCount{0};
    Kwi::Session session;
    QTemporaryDir directory;
    QQuickWindow window;
    Kwi::TerminalView view;
};

}

class ImeTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
#ifdef Q_OS_MACOS
    void init()
    {
        activateMacTestApplication();
    }
#endif
    void compositionLifecycle();
    void processLifecycle();
    void focusSessionAndReadOnly();
};

// IME composition keeps preedit local, sends committed text and restores the caret on cancellation.
void ImeTest::compositionLifecycle()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    const QRectF anchor{fixture.caret()};
    const QString history{fixture.session.historyText()};

    for (const QString &preedit : {QStringLiteral("ㅎ"), QStringLiteral("하"), QStringLiteral("한"), QStringLiteral("하"), QStringLiteral("한")}) {
        fixture.compose(preedit);
        QTRY_COMPARE(fixture.caret().x(), anchor.x() + 2 * anchor.width());
        QCOMPARE(fixture.session.historyText(), history);
        QVERIFY(fixture.captured().isEmpty());
    }

    // A syllable commit and the next preedit may arrive in the same event.
    fixture.compose(QStringLiteral("글"), {}, QStringLiteral("한"));
    QTRY_COMPARE(fixture.captured(), QStringLiteral("한").toUtf8());
    QCOMPARE(fixture.caret().x(), anchor.x() + 2 * anchor.width());
    fixture.compose({}, {}, QStringLiteral("글"));
    QTRY_COMPARE(fixture.captured(), QStringLiteral("한글").toUtf8());
    QCOMPARE(fixture.caret(), anchor); // The raw child deliberately does not echo.
    fixture.compose(QStringLiteral("取消"));
    fixture.compose({});
    QCOMPARE(fixture.caret(), anchor);
    QCOMPARE(fixture.captured(), QStringLiteral("한글").toUtf8());

    // Terminal output must not become editable surrounding text.
    QCOMPARE(fixture.view.inputMethodQuery(Qt::ImSurroundingText).toString(), QString());
    QCOMPARE(fixture.view.inputMethodQuery(Qt::ImCursorPosition).toInt(), 0);
    QCOMPARE(fixture.view.inputMethodQuery(Qt::ImAnchorPosition).toInt(), 0);
}

// IME input is enabled only while the process runs, and exit clears unfinished composition.
void ImeTest::processLifecycle()
{
    InputFixture fixture;
    QVERIFY(!fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    const QRectF initialCaret{fixture.caret()};
    fixture.compose(QStringLiteral("未"), {}, QStringLiteral("未"));
    QCOMPARE(fixture.caret(), initialCaret);

    connect(&fixture.session, &Kwi::Session::stateChanged, &fixture.view, [&] {
        const auto state{fixture.session.state()};
        QCOMPARE(fixture.view.inputMethodQuery(Qt::ImEnabled).toBool(), state == Kwi::Session::State::Running);

        if (state != Kwi::Session::State::Running) {
            const QRectF caret{fixture.caret()};
            fixture.compose(QStringLiteral("遅"), {}, QStringLiteral("遅"));
            QCOMPARE(fixture.caret(), caret);
        }
    });

    QVERIFY(fixture.start());
    const QRectF anchor{fixture.caret()};
    fixture.compose(QStringLiteral("한"));
    QVERIFY(fixture.caret() != anchor);
    QVERIFY(fixture.requestExit());
    QTRY_COMPARE(fixture.session.state(), Kwi::Session::State::Exited);
    QCOMPARE(fixture.caret(), anchor);
    QVERIFY(fixture.captured().isEmpty());

    QVERIFY(fixture.session.start());
    QVERIFY(fixture.waitForReady());
    QVERIFY(fixture.captured().isEmpty());
    fixture.compose({}, {}, QStringLiteral("new"));
    QTRY_COMPARE(fixture.captured(), QByteArray("new"));
    const QRectF replacementAnchor{fixture.caret()};
    fixture.compose(QStringLiteral("한"));
    fixture.session.close();
    QTRY_COMPARE(fixture.session.state(), Kwi::Session::State::Exited);
    QCOMPARE(fixture.caret(), replacementAnchor);
    QCOMPARE(fixture.captured(), QByteArray("new"));
}

// Losing focus, detaching the session or enabling read-only mode discards unfinished IME composition.
void ImeTest::focusSessionAndReadOnly()
{
    InputFixture fixture;
    QVERIFY(fixture.start());
    const QRectF anchor{fixture.caret()};
    fixture.compose(QStringLiteral("日本語"));
    QVERIFY(fixture.caret() != anchor);
    QQuickItem other{fixture.window.contentItem()};
    other.forceActiveFocus();
    QTRY_VERIFY(!fixture.view.hasActiveFocus());
    QCOMPARE(fixture.caret(), anchor);
    fixture.view.forceActiveFocus();
    QTRY_VERIFY(fixture.view.hasActiveFocus());
    QCOMPARE(fixture.caret(), anchor);

    fixture.compose(QStringLiteral("中文"));
    fixture.session.setReadOnly(true);
    QVERIFY(!fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    QCOMPARE(fixture.caret(), anchor);
    fixture.compose(QStringLiteral("中"), {}, QStringLiteral("文"));
    QCOMPARE(fixture.caret(), anchor);
    fixture.session.setReadOnly(false);
    QVERIFY(fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    fixture.compose(QStringLiteral("한"));
    fixture.view.setSession(nullptr);
    QVERIFY(!fixture.view.inputMethodQuery(Qt::ImEnabled).toBool());
    fixture.view.setSession(&fixture.session);
    QTRY_COMPARE(fixture.caret(), anchor);
    fixture.session.feed("\x1b[5n");
    QTRY_COMPARE(fixture.captured(), QByteArray("\x1b[0n"));
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    ImeTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "imetest.moc"
