// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"
#include "Terminal.h"

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringLiteral>
#include <QTest>

class ClipboardTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void init()
    {
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("clipboard-original"));

        if (clipboard->supportsSelection()) {
            clipboard->setText(QStringLiteral("selection-original"), QClipboard::Selection);
        }
    }

    void writesAndClearsSelectedDestination_data()
    {
        QTest::addColumn<QByteArray>("selector");
        QTest::addColumn<bool>("selection");
        QTest::newRow("clipboard") << QByteArray("c") << false;
        QTest::newRow("default") << QByteArray() << false;
        QTest::newRow("primary") << QByteArray("p") << true;
    }

    // Complete OSC 52 sequences write or clear the requested clipboard while preserving the other clipboard.
    void writesAndClearsSelectedDestination()
    {
        QFETCH(QByteArray, selector);
        QFETCH(bool, selection);
        Kwi::Session session;
        auto *clipboard = QGuiApplication::clipboard();

        if (selection && !clipboard->supportsSelection()) {
            QSKIP("This platform does not support the primary selection.");
        }

        const auto mode = selection ? QClipboard::Selection : QClipboard::Clipboard;
        const auto otherMode = selection ? QClipboard::Clipboard : QClipboard::Selection;
        const QString original = clipboard->text(mode);
        const QString otherOriginal = clipboard->supportsSelection() ? clipboard->text(otherMode) : QString();
        const QString text = QStringLiteral("tmux: Grüß dich — 한국어 👻\nsecond line");
        const QByteArray sequence = QByteArray("\x1b]52;") + selector + ';' + text.toUtf8().toBase64() + "\x1b\\";

        // PTY reads may split a sequence anywhere. Wait for the OSC terminator before writing.
        for (qsizetype index = 0; index < sequence.size() - 2; ++index) {
            session.feed(sequence.mid(index, 1));
            QCOMPARE(clipboard->text(mode), original);
        }

        session.feed(sequence.mid(sequence.size() - 2, 1));
        session.feed(sequence.last(1));
        QCOMPARE(clipboard->text(mode), text);

        if (clipboard->supportsSelection()) {
            QCOMPARE(clipboard->text(otherMode), otherOriginal);
        }

        QVERIFY(session.historyText().trimmed().isEmpty());

        session.feed(QByteArray("\x1b]52;") + selector + ";\x07");
        QVERIFY(clipboard->text(mode).isEmpty());

        if (clipboard->supportsSelection()) {
            QCOMPARE(clipboard->text(otherMode), otherOriginal);
        }
    }

    // OSC 52 and Kitty clipboard read requests return no clipboard contents.
    void readsRemainDisabled()
    {
        Kwi::Session session;
        QSignalSpy replies{session.terminal(), &GhosttyQt::Terminal::writeRequested};
        session.feed("\x1b]52;c;?\x07\x1b]52;p;?\x1b\\\x1b]52;s;?\x1b\\");
        QVERIFY(replies.isEmpty());
        session.feed("\x1b]5522;type=read:id=test;dGV4dC9wbGFpbg==\x1b\\");
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.constFirst().constFirst().toByteArray(), QByteArray("\x1b]5522;type=read:status=EPERM:id=test\x1b\\"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("clipboard-original"));

        if (QGuiApplication::clipboard()->supportsSelection()) {
            QCOMPARE(QGuiApplication::clipboard()->text(QClipboard::Selection), QStringLiteral("selection-original"));
        }
    }

    // Kitty clipboard writes publish their MIME formats together and acknowledge completion.
    void kittyWritesAreAtomicAndAcknowledged()
    {
        Kwi::Session session;
        auto *clipboard = QGuiApplication::clipboard();
        QSignalSpy replies{session.terminal(), &GhosttyQt::Terminal::writeRequested};
        session.feed(
            "\x1b]5522;type=write:id=copy\x1b\\"
            "\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGVsbG8=\x1b\\"
            "\x1b]5522;type=wdata:mime=dGV4dC9odG1s;PGI+aGVsbG88L2I+\x1b\\");
        QCOMPARE(clipboard->text(), QStringLiteral("clipboard-original"));
        QVERIFY(replies.isEmpty());

        session.feed("\x1b]5522;type=wdata\x1b\\");
        QCOMPARE(clipboard->text(), QStringLiteral("hello"));
        QCOMPARE(clipboard->mimeData()->html(), QStringLiteral("<b>hello</b>"));
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies.takeFirst().constFirst().toByteArray(), QByteArray("\x1b]5522;type=write:status=DONE:id=copy\x1b\\"));
    }

    // Clipboard escape sequences from a running child reach the desktop clipboard.
    void childOutputWritesClipboard()
    {
        Kwi::Session session;
        session.setExecutable(QStringLiteral("/bin/sh"));
        session.setArguments({QStringLiteral("-c"), QStringLiteral("printf '\\033]52;c;ZnJvbSB0aGUgUFRZ\\007'; read -r acknowledgment")});
        QVERIFY(session.start());
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("from the PTY"));
        QVERIFY(session.sendInput(QStringLiteral("\n")));
        QTRY_VERIFY(session.isFinished());
        QCOMPARE(session.exitCode(), 0);
    }
};

QTEST_MAIN(ClipboardTest)
#include "clipboardtest.moc"
