// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Search.h"
#include "Frame.h"
#include "Terminal.h"

#include <QByteArray>
#include <QObject>
#include <QRect>
#include <QRegion>
#include <QString>
#include <QStringLiteral>
#include <QTest>

#include <memory>
#include <optional>

using namespace GhosttyQt;

namespace
{
Search::Status finish(Search &search)
{
    auto status = search.feed();

    for (int step = 0; step < 10000; ++step) {
        if (status == Search::Status::Complete || status == Search::Status::Error) {
            return status;
        }

        status = status == Search::Status::FeedRequired ? search.feed() : search.tick();
    }

    return status;
}

int rowStartingWith(const Frame &frame, const QString &prefix)
{
    for (int y{0}; y < frame.rows.size(); ++y) {
        QString text;

        for (const auto &cell : frame.rows.at(y))
            text += cell.text;

        if (text.startsWith(prefix))
            return y;
    }

    return -1;
}
}

class SearchTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void literalMatching_data();
    void literalMatching();
    void navigationTracksLiveOutput();
    void navigationScrollsBothWaysAndWraps();
    void visibleMatchesKeepViewport();
    void viewportMatchesUnicode_data();
    void viewportMatchesUnicode();
    void viewportMatchesWrappedAndClipped();
    void viewportMatchesFollowResize();
    void reflowAndEviction();
    void activeScreenAndReset();
    void terminalCanBeDestroyedFirst();
};

void SearchTest::literalMatching_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<qsizetype>("count");
    QTest::newRow("ascii-folding") << QStringLiteral("alpha") << qsizetype{2};
    QTest::newRow("unicode-exact") << QStringLiteral("älpha") << qsizetype{1};
    QTest::newRow("wide-combining") << QStringLiteral("界é") << qsizetype{1};
    QTest::newRow("literal-punctuation") << QStringLiteral("[a+]+") << qsizetype{1};
    QTest::newRow("missing") << QStringLiteral("missing") << qsizetype{0};
}

// Literal searches count and select ASCII, Unicode and punctuation matches and clear selection when no match exists.
void SearchTest::literalMatching()
{
    QFETCH(QString, query);
    QFETCH(qsizetype, count);
    Terminal terminal{12, 8};
    QVERIFY(terminal.feed(QStringLiteral("alpha ALPHA älpha Älpha\r\n界é [a+]+").toUtf8()));
    auto search = terminal.createSearch();
    QVERIFY(search && search->isValid());
    QVERIFY(search->setNeedle(query));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, count);

    if (count > 0) {
        QVERIFY(search->select(true));
        QVERIFY(search->applySelection());
        QVERIFY(!terminal.selectionText().isEmpty());
        QCOMPARE(terminal.selectionText(), query == QStringLiteral("alpha") ? QStringLiteral("ALPHA") : query);
        const auto index = search->results().selectedIndex;
        QVERIFY(index && *index >= 0 && *index < count);
    } else {
        QVERIFY(!search->results().selectedIndex);
        QVERIFY(search->applySelection());
        QVERIFY(terminal.selectionText().isEmpty());
    }
}

// Search navigation retains the selected match as new output changes result indices.
void SearchTest::navigationTracksLiveOutput()
{
    Terminal terminal{30, 8};
    QVERIFY(terminal.feed("marker\r\nMARKER\r\nMarker\r\n"));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("marker")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 3);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("Marker"));
    QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{0});
    QVERIFY(terminal.feed("ordinary output\r\n\x1b]2;updated title\x07"));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("MARKER"));
    QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{1});
    QVERIFY(terminal.feed("mArKeR new output\r\n"));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 4);
    QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{2});
    QCOMPARE(terminal.selectionText(), QStringLiteral("MARKER"));
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("marker"));
    QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{3});
    QVERIFY(search->select(false));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("MARKER"));
    QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{2});
}

// Search navigation wraps in both directions, scrolls each match into view and selects its cells.
void SearchTest::navigationScrollsBothWaysAndWraps()
{
    Terminal terminal{24, 3};
    const QByteArray output{QByteArray("first needle\r\n") + QByteArray("filler\r\n").repeated(5) + "second needle\r\n" + QByteArray("filler\r\n").repeated(5)
                            + "third needle\r\n" + QByteArray("filler\r\n").repeated(5) + "bottom"};
    QVERIFY(terminal.feed(output));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("needle")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 3);
    Frame frame;
    QVERIFY(terminal.snapshot(frame));
    quint64 offset{frame.scrollOffset};
    struct Step {
        bool forward{false};
        QString label;
        bool downward{false};
        qsizetype index{0};
    };
    const Step steps[]{{true, QStringLiteral("third"), false, 0},
                       {true, QStringLiteral("second"), false, 1},
                       {true, QStringLiteral("first"), false, 2},
                       {true, QStringLiteral("third"), true, 0},
                       {false, QStringLiteral("first"), false, 2},
                       {false, QStringLiteral("second"), true, 1},
                       {false, QStringLiteral("third"), true, 0},
                       {false, QStringLiteral("first"), false, 2}};

    for (const auto &step : steps) {
        QVERIFY(search->select(step.forward));
        QCOMPARE(search->results().selectedIndex, std::optional<qsizetype>{step.index});
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(step.downward ? frame.scrollOffset > offset : frame.scrollOffset < offset);
        offset = frame.scrollOffset;
        const int row{rowStartingWith(frame, step.label + QStringLiteral(" needle"))};
        QVERIFY(row >= 0);
        QVERIFY(search->applySelection());
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, offset);
        QCOMPARE(terminal.selectionText(), QStringLiteral("needle"));

        for (qsizetype x{step.label.size() + 1}; x < step.label.size() + 7; ++x)
            QVERIFY(frame.rows.at(row).at(x).flags & Selected);
    }
}

// Selecting a match already in view preserves the viewport position.
void SearchTest::visibleMatchesKeepViewport()
{
    Terminal terminal{24, 5};
    QVERIFY(terminal.feed("head\r\nfirst needle\r\nsecond needle\r\ntail"));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("needle")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 2);
    Frame frame;
    QVERIFY(terminal.snapshot(frame));
    const quint64 offset{frame.scrollOffset};

    for (const QString &label : {QStringLiteral("second"), QStringLiteral("first"), QStringLiteral("second")}) {
        QVERIFY(search->select(true));
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, offset);
        const int row{rowStartingWith(frame, label + QStringLiteral(" needle"))};
        QVERIFY(row >= 0);
        QVERIFY(search->applySelection());
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, offset);
        QCOMPARE(terminal.selectionText(), QStringLiteral("needle"));
        QVERIFY(frame.rows.at(row).at(label.size() + 1).flags & Selected);
    }
}

void SearchTest::viewportMatchesUnicode_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("query");
    QTest::addColumn<QRect>("cells");
    QTest::newRow("wide") << QStringLiteral("x界éz") << QStringLiteral("界") << QRect{1, 0, 2, 1};
    QTest::newRow("combining") << QStringLiteral("x界éz") << QStringLiteral("é") << QRect{3, 0, 1, 1};
    QTest::newRow("wide-and-combining") << QStringLiteral("x界éz") << QStringLiteral("界é") << QRect{1, 0, 3, 1};
    QTest::newRow("emoji-zwj") << QStringLiteral("x👩‍💻z") << QStringLiteral("👩‍💻") << QRect{1, 0, 2, 1};
}

// Search highlights cover the terminal cells occupied by wide, combining and joined Unicode characters.
void SearchTest::viewportMatchesUnicode()
{
    QFETCH(QString, text);
    QFETCH(QString, query);
    QFETCH(QRect, cells);
    Terminal terminal{20, 3};
    QVERIFY(terminal.feed(text.toUtf8()));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(query));
    QVERIFY(search->feed() != Search::Status::Error);
    const auto matches = search->viewportMatches();
    QVERIFY(matches);
    QCOMPARE(*matches, QRegion(cells));
}

// Search highlights follow wrapped text and clip to the visible rows as the viewport scrolls.
void SearchTest::viewportMatchesWrappedAndClipped()
{
    Terminal terminal{8, 3};
    const QString query{QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL")};
    const QByteArray output{"zz" + query.toUtf8() + "!!"};
    QVERIFY(terminal.feed(output));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(query));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 1);

    struct Viewport {
        quint64 offset{0};
        QRegion cells;
    };
    const Viewport viewports[]{{0, QRegion(QRect{2, 0, 6, 1}) + QRect{0, 1, 8, 2}}, {1, QRegion(QRect{0, 0, 8, 3})}, {3, QRegion(QRect{0, 0, 8, 2})}};

    for (const auto &viewport : viewports) {
        terminal.scrollTo(viewport.offset);
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.scrollOffset, viewport.offset);
        QVERIFY(search->feed() != Search::Status::Error);
        const auto matches = search->viewportMatches();
        QVERIFY(matches);
        QCOMPARE(*matches, viewport.cells);
    }
}

// Search highlights follow text as resizing wraps and unwraps a match.
void SearchTest::viewportMatchesFollowResize()
{
    Terminal terminal{10, 4};
    QVERIFY(terminal.feed("0123needle"));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("needle")));
    QVERIFY(search->feed() != Search::Status::Error);
    auto matches = search->viewportMatches();
    QVERIFY(matches);
    QCOMPARE(*matches, QRegion(QRect{4, 0, 6, 1}));

    QVERIFY(terminal.resize(6, 4));
    terminal.scrollTo(0);
    QVERIFY(search->feed() != Search::Status::Error);
    matches = search->viewportMatches();
    QVERIFY(matches);
    const QRegion wrapped{QRegion(QRect{4, 0, 2, 1}) + QRect{0, 1, 4, 1}};
    QCOMPARE(*matches, wrapped);

    QVERIFY(terminal.resize(10, 4));
    terminal.scrollTo(0);
    QVERIFY(search->feed() != Search::Status::Error);
    matches = search->viewportMatches();
    QVERIFY(matches);
    QCOMPARE(*matches, QRegion(QRect{4, 0, 6, 1}));
}

// Matches survive reflow, disappear when their history is evicted and reappear when matching output arrives.
void SearchTest::reflowAndEviction()
{
    Terminal terminal{24, 5, 1};
    QVERIFY(terminal.feed(QStringLiteral("prefix 界é needle suffix\r\n").toUtf8()));
    auto search = terminal.createSearch();
    const QString query{QStringLiteral("界é needle")};
    QVERIFY(search->setNeedle(query));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), query);
    QVERIFY(terminal.resize(7, 5));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 1);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), query);
    QVERIFY(terminal.feed(QByteArray("ordinary output\r\n").repeated(2000)));
    QVERIFY(!terminal.historyText().contains(query));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 0);
    QVERIFY(!search->results().selectedIndex);
    QVERIFY(search->applySelection());
    QVERIFY(terminal.selectionText().isEmpty());
    QVERIFY(terminal.feed(query.toUtf8()));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 1);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), query);
}

// Search follows the active screen and clears results and selection after reset.
void SearchTest::activeScreenAndReset()
{
    Terminal terminal{30, 5};
    QVERIFY(terminal.feed("primary marker"));
    auto search = terminal.createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("primary")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("primary"));
    QVERIFY(terminal.feed("\x1b[?1049h\x1b[2J\x1b[Halternate marker"));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 0);
    QVERIFY(search->applySelection());
    QVERIFY(terminal.selectionText().isEmpty());
    QVERIFY(search->setNeedle(QStringLiteral("alternate")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QVERIFY(search->select(true));
    QVERIFY(search->applySelection());
    QCOMPARE(terminal.selectionText(), QStringLiteral("alternate"));
    QVERIFY(terminal.feed("\x1b[?1049l"));
    QVERIFY(search->setNeedle(QStringLiteral("primary")));
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 1);
    terminal.reset();
    QCOMPARE(finish(*search), Search::Status::Complete);
    QCOMPARE(search->results().totalMatches, 0);
    QVERIFY(search->applySelection());
    QVERIFY(terminal.selectionText().isEmpty());
}

// Search operations return errors or empty results after their terminal is destroyed.
void SearchTest::terminalCanBeDestroyedFirst()
{
    auto terminal = std::make_unique<Terminal>(80, 4);
    QVERIFY(terminal->feed(QByteArray("history needle\r\n").repeated(1000)));
    auto search = terminal->createSearch();
    QVERIFY(search->setNeedle(QStringLiteral("needle")));
    QVERIFY(search->feed() != Search::Status::Error);
    terminal.reset();
    QCOMPARE(search->feed(), Search::Status::Error);
    QCOMPARE(search->tick(), Search::Status::Error);
    QVERIFY(!search->applySelection());
    QVERIFY(!search->viewportMatches());
    search.reset();
}

QTEST_GUILESS_MAIN(SearchTest)
#include "searchtest.moc"
