// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "HistoryReader.h"
#include "Terminal.h"

#include <QByteArray>
#include <QObject>
#include <QTest>

#include <memory>

using namespace GhosttyQt;

class HistoryTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void readsRetainedText_data();
    void readsRetainedText();
    void rejectsChangedHistory_data();
    void rejectsChangedHistory();
    void readerOutlivesTerminal();
};

void HistoryTest::readsRetainedText_data()
{
    QTest::addColumn<QByteArray>("inputLine");
    QTest::addColumn<QByteArray>("textLine");
    QTest::newRow("plain") << QByteArray("line of text\r\n") << QByteArray("line of text\n");
    QTest::newRow("unicode-and-color") << QByteArray("\x1b[31mline 界é\x1b[0m\r\n") << QByteArray("line 界é\n");
}

// History readers return ordered UTF-8 chunks from the captured output and exclude later additions.
void HistoryTest::readsRetainedText()
{
    QFETCH(QByteArray, inputLine);
    QFETCH(QByteArray, textLine);
    Terminal terminal{80, 4, Terminal::UnlimitedScrollback};
    const QByteArray input{inputLine.repeated(9000) + "last"};
    QVERIFY(terminal.feed(input));
    const QByteArray expected{textLine.repeated(9000) + "last"};
    auto reader = terminal.createHistoryReader();
    QVERIFY(reader);

    QVERIFY(terminal.feed("\r\nlater output"));
    QByteArray actual;
    int chunks{0};

    for (;;) {
        const auto chunk = reader->read();
        QVERIFY(chunk.has_value());
        QVERIFY(chunk->finished || !chunk->bytes.isEmpty());
        actual += chunk->bytes;
        QVERIFY(expected.startsWith(actual));
        ++chunks;

        if (chunk->finished) {
            break;
        }
    }

    QVERIFY(chunks > 1);
    QCOMPARE(actual, expected);
}

void HistoryTest::rejectsChangedHistory_data()
{
    QTest::addColumn<QByteArray>("change");
    QTest::newRow("resize") << QByteArray("resize");
    QTest::newRow("reset") << QByteArray("reset");
    QTest::newRow("clear") << QByteArray("clear");
    QTest::newRow("alternate-screen") << QByteArray("alternate");
}

// History readers report invalidation after resize, reset, scrollback clearing or a screen switch.
void HistoryTest::rejectsChangedHistory()
{
    QFETCH(QByteArray, change);
    Terminal terminal{80, 4, Terminal::UnlimitedScrollback};
    QVERIFY(terminal.feed(QByteArray("retained line\r\n").repeated(100)));
    auto reader = terminal.createHistoryReader();
    QVERIFY(reader);

    if (change == "resize") {
        QVERIFY(terminal.resize(40, 4));
    } else if (change == "reset") {
        terminal.reset();
    } else if (change == "clear") {
        terminal.clearScrollback();
    } else {
        QVERIFY(terminal.feed("\x1b[?1049h"));
    }

    const auto result = reader->read();
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), HistoryReader::Error::HistoryChanged);
}

// A history reader reports TerminalUnavailable after its terminal is destroyed.
void HistoryTest::readerOutlivesTerminal()
{
    auto terminal = std::make_unique<Terminal>();
    auto reader = terminal->createHistoryReader();
    QVERIFY(reader);
    terminal.reset();

    const auto result = reader->read();
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), HistoryReader::Error::TerminalUnavailable);
}

QTEST_GUILESS_MAIN(HistoryTest)

#include "historytest.moc"
