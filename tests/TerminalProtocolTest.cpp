// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "HistoryReader.h"
#include "Terminal.h"
#include <QFile>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace
{
QString visible(const GhosttyQt::Frame &frame)
{
    QString text;
    for (const auto &row : frame.rows) {
        for (const auto &cell : row)
            text += cell.text;
        text += u'\n';
    }
    return text;
}

QByteArray graphics(const QByteArray &control, const QByteArray &pixels = {})
{
    return "\033_G" + control + (pixels.isEmpty() ? QByteArray{} : ";" + pixels.toBase64()) + "\033\\";
}
}

class TerminalProtocolTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void synchronizedOutputReleaseAndTimeout()
    {
        GhosttyQt::Terminal terminal{40, 6};
        QVERIFY(terminal.feed("before"));
        GhosttyQt::Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(visible(frame).contains(QLatin1String("before")));
        QVERIFY(terminal.feed("\033[?2026h\r\nafter"));
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(!visible(frame).contains(QLatin1String("after")));
        QVERIFY(terminal.feed("\033[?2026l"));
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(visible(frame).contains(QLatin1String("after")));
        QVERIFY(terminal.feed("\033[?2026h\r\ntimeout"));
        QTRY_VERIFY_WITH_TIMEOUT(!terminal.isModeEnabled(GhosttyQt::Mode::SynchronizedOutput), 1000);
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(visible(frame).contains(QLatin1String("timeout")));
        QVERIFY(terminal.feed("\r\nstill usable"));
        QVERIFY(terminal.historyText().contains(QLatin1String("still usable")));
    }

    void terminalReportsAndScreenModes()
    {
        GhosttyQt::Terminal terminal{80, 24};
        QVERIFY(terminal.resize(80, 24, {9, 18}));
        terminal.setTerminfoName("xterm-256color");
        terminal.setApplicationIdentity("Kwi phase6");
        QByteArray replies;
        connect(&terminal, &GhosttyQt::Terminal::writeRequested, this, [&](const QByteArray &bytes) {
            replies += bytes;
        });
        QVERIFY(terminal.feed("\033[5n\033[3;7H\033[6n\033[18t\033[14t\033[16t\033[>q\033[c\033[>c"));
        QVERIFY2(replies.contains("\033[0n"), replies.toHex().constData());
        QVERIFY(replies.contains("\033[3;7R"));
        QVERIFY(replies.contains("\033[8;24;80t"));
        QVERIFY(replies.contains("\033[4;432;720t"));
        QVERIFY(replies.contains("\033[6;18;9t"));
        QVERIFY(replies.contains("Kwi phase6"));
        QVERIFY(replies.contains("\033[?62;22c"));
        QVERIFY(replies.contains("\033[>1;10;0c"));
        QVERIFY(terminal.feed("\033[2J\033[Hprimary\033[?1049halternate"));
        QVERIFY(terminal.isModeEnabled(GhosttyQt::Mode::AlternateScreen));
        QVERIFY(terminal.historyText().contains(QLatin1String("alternate")));
        QVERIFY(terminal.feed("\033[?1049l"));
        QVERIFY(!terminal.isModeEnabled(GhosttyQt::Mode::AlternateScreen));
        QVERIFY(terminal.historyText().contains(QLatin1String("primary")));
        QVERIFY(!terminal.historyText().contains(QLatin1String("alternate")));
        terminal.setColors(Qt::white, Qt::black, {});
        replies.clear();
        QVERIFY(terminal.feed("\033]10;?\033\\\033]11;?\033\\"));
        QVERIFY(replies.contains("rgb:ffff/ffff/ffff"));
        QVERIFY(replies.contains("rgb:0000/0000/0000"));
    }

    void clipboardWritesDoNotReadOrTouchSystemClipboard()
    {
        GhosttyQt::Terminal terminal;
        QByteArray contents;
        QList<QClipboard::Mode> destinations;
        terminal.setClipboardWriteHandler([&](std::unique_ptr<QMimeData> data, QClipboard::Mode mode) {
            destinations.append(mode);
            contents = data ? data->data(QStringLiteral("text/plain")) : QByteArray{};
            return mode == QClipboard::Clipboard;
        });
        const QByteArray text = QStringLiteral("clipboard-é & literal $HOME").toUtf8();
        QByteArray sequence = "\033]52;c;" + text.toBase64() + "\033\\";
        QVERIFY(terminal.feed(sequence));
        QCOMPARE(contents, text);
        QCOMPARE(destinations, QList<QClipboard::Mode>{QClipboard::Clipboard});
        sequence = "\033]52;p;" + text.toBase64() + "\033\\";
        QVERIFY(terminal.feed(sequence));
        QCOMPARE(destinations.last(), QClipboard::Selection);
        const auto count = destinations.size();
        QVERIFY(terminal.feed("\033]52;c;?\033\\"));
        QCOMPARE(destinations.size(), count); // No clipboard-read callback is installed.
        QVERIFY(terminal.feed("\033]52;c;\033\\"));
        QVERIFY(contents.isEmpty());
        const QByteArray bounded(16 * 1024, 'x');
        sequence = "\033]52;c;" + bounded.toBase64() + "\033\\";
        QVERIFY(terminal.feed(sequence));
        QCOMPARE(contents, bounded);
    }

    void promptsSelectionAndExplicitLinks()
    {
        GhosttyQt::Terminal terminal{30, 5, 1000};
        QVERIFY(terminal.feed("\033]133;A\a$ \033]133;B\aecho first\r\n\033]133;C\afirst output\r\n\033]133;D;0\a"));
        QVERIFY(terminal.feed("\033]133;A\a$ \033]133;B\aecho second\r\n\033]133;C\asecond output\r\n\033]133;D;0\a"));
        QVERIFY(terminal.feed("\033]133;A\a$ \033]133;B\a"));
        QCOMPARE(terminal.commandOutput().trimmed(), QStringLiteral("second output"));
        for (int n = 0; n < 20; ++n)
            QVERIFY(terminal.feed("\r\nline"));
        GhosttyQt::Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const auto bottom = frame.scrollOffset;
        terminal.scrollPrompt(-1);
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.scrollOffset < bottom);
        terminal.scrollBottom();
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, bottom);
        terminal.reset();
        QVERIFY(terminal.feed("\033]8;;https://example.com/path?q=literal\033\\link\033]8;;\033\\"));
        QCOMPARE(terminal.linkAt({1, 0}), QStringLiteral("https://example.com/path?q=literal"));
        terminal.reset();
        QVERIFY(terminal.feed(QStringLiteral("A中e\u0301Z\r\n12345").toUtf8()));
        QVERIFY(terminal.select({0, 0}, {4, 0}));
        QCOMPARE(terminal.selectionText(), QStringLiteral("A中e\u0301Z"));
        QVERIFY(terminal.select({0, 0}, {0, 1}, true));
        QCOMPARE(terminal.selectionText(), QStringLiteral("A\n1"));
        terminal.clearSelection();
        QVERIFY(!terminal.hasSelection());
    }

    void historyReadersInvalidateSafely()
    {
        auto terminal = std::make_unique<GhosttyQt::Terminal>(40, 6, 1000);
        for (int n = 0; n < 200; ++n)
            QVERIFY(terminal->feed("a known line\r\n"));
        auto reader = terminal->createHistoryReader();
        QVERIFY(reader);
        QVERIFY(reader->read());
        QVERIFY(terminal->resize(45, 6));
        auto resized = reader->read();
        QVERIFY(!resized);
        QCOMPARE(resized.error(), GhosttyQt::HistoryReader::Error::HistoryChanged);
        reader = terminal->createHistoryReader();
        terminal.reset();
        auto closed = reader->read();
        QVERIFY(!closed);
        QCOMPARE(closed.error(), GhosttyQt::HistoryReader::Error::TerminalUnavailable);
    }

    void directImagesLayersAnimationAndDelete()
    {
        // Fixtures follow https://sw.kovidgoyal.net/kitty/graphics-protocol/.
        GhosttyQt::Terminal terminal{40, 8};
        const QByteArray red = QByteArray::fromHex("ff0000ff"), blue = QByteArray::fromHex("0000ffff");
        QVERIFY(terminal.feed(graphics("a=T,f=32,s=1,v=1,i=1,p=11,z=-1073741825,q=2", red)));
        QVERIFY(terminal.feed(graphics("a=T,f=32,s=1,v=1,i=2,p=12,z=1,q=2", red)));
        GhosttyQt::Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.images.size(), 2);
        QCOMPARE(frame.images.first().layer, GhosttyQt::ImageLayer::BelowBackground);
        QCOMPARE(frame.images.last().layer, GhosttyQt::ImageLayer::AboveText);
        const auto generation = frame.images.last().generation;
        QVERIFY(terminal.feed(graphics("a=f,f=32,s=1,v=1,i=2,z=30,q=2", blue)));
        QVERIFY(terminal.feed(graphics("a=a,i=2,c=2,q=2")));
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.images.last().image.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(frame.images.last().generation != generation);
        QVERIFY(terminal.feed(graphics("a=a,i=2,r=1,z=30,q=2")));
        QVERIFY(terminal.feed(graphics("a=a,i=2,r=2,z=30,q=2")));
        QVERIFY(terminal.feed(graphics("a=a,i=2,s=3,q=2")));
        QTRY_VERIFY_WITH_TIMEOUT(terminal.snapshot(frame) && frame.images.last().image.pixelColor(0, 0) == QColor(Qt::red), 1000);
        QVERIFY(terminal.feed(graphics("a=a,i=2,s=1,q=2")));
        QVERIFY(terminal.resize(45, 10, {9, 18}));
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.images.size(), 2);
        QVERIFY(terminal.feed(graphics("a=d,d=I,i=2,q=2")));
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.images.size(), 1);
        QCOMPARE(frame.images.first().imageId, 1);
        QVERIFY(terminal.feed(graphics("a=d,d=A,q=2")));
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.images.isEmpty());
    }

    void deniedImageTransports()
    {
        GhosttyQt::Terminal terminal;
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("tty-graphics-protocol-valid-rgba-image"));
        {
            QFile file{path};
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(QByteArray::fromHex("ff0000ff")), 4);
        }
        QByteArray replies;
        connect(&terminal, &GhosttyQt::Terminal::writeRequested, this, [&](const QByteArray &bytes) {
            replies += bytes;
        });
        for (const auto medium : {'f', 't', 's'}) {
            replies.clear();
            const QByteArray command = "a=T,t=" + QByteArray(1, medium) + ",f=32,s=1,v=1,i=7";
            QVERIFY(terminal.feed(graphics(command, medium != 's' ? QFile::encodeName(path) : QByteArray("/kwi-phase6-nonexistent-shm"))));
            QVERIFY2(replies.contains("EINVAL: unsupported medium"), replies.constData());
            GhosttyQt::Frame frame;
            QVERIFY(terminal.snapshot(frame));
            QVERIFY(frame.images.isEmpty());
        }
        replies.clear();
        QVERIFY(terminal.feed("\033_25a1;s\033\\\033_25a1;q;cp=0041\033\\"));
        QVERIFY(replies.isEmpty()); // Source-disabled glyph protocol stays disabled.
    }
};

QTEST_GUILESS_MAIN(TerminalProtocolTest)
#include "TerminalProtocolTest.moc"
