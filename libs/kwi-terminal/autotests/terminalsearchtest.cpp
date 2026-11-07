// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalSearch.h"
#include "Frame.h"
#include "Session.h"
#include "Terminal.h"
#include "TerminalView.h"

#include <KLocalizedString>
#include <QAccessible>
#include <QAccessibleTextInterface>
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLatin1Char>
#include <QList>
#include <QMap>
#include <QObject>
#include <QQuickWindow>
#include <QRectF>
#include <QSignalSpy>
#include <QSizeF>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtMath>
#include <chrono>

using namespace Kwi;

namespace
{
class ViewportFixture
{
public:
    ViewportFixture()
        : view(window.contentItem())
    {
        window.resize(640, 400);
        view.setSize(QSizeF(window.size()));
        view.setSession(&session);
        window.show();
    }

    bool prepare()
    {
        if (!QTest::qWaitForWindowExposed(&window)) {
            return false;
        }

        const QRectF cell{view.inputMethodQuery(Qt::ImCursorRectangle).toRectF()};
        view.setSize(QSizeF(40 * cell.width() + 2 * cell.x(), 10 * cell.height() + 2 * cell.y()));
        window.resize(qCeil(view.width()), qCeil(view.height()));

        return QTest::qWaitFor([this] {
            GhosttyQt::Frame frame;

            return session.snapshot(frame) && frame.columns == 40 && frame.rows.size() == 10;
        });
    }

    QAccessibleTextInterface *text()
    {
        auto *accessible = QAccessible::queryAccessibleInterface(&view);

        return accessible ? accessible->textInterface() : nullptr;
    }

    QString visibleText()
    {
        const auto *content = text();

        return content ? content->text(0, -1) : QString();
    }

    QString visibleSelection()
    {
        const auto *content = text();

        if (!content || content->selectionCount() != 1) {
            return {};
        }

        int start{-1};
        int end{-1};
        content->selection(0, &start, &end);

        return content->text(start, end);
    }

    qint64 viewportOffset() const
    {
        return qRound64(view.scrollPosition() / view.scrollStepSize());
    }

    Session session;
    QQuickWindow window;
    TerminalView view;
};

QString rowLabel(int row)
{
    return QStringLiteral("row-%1").arg(row, 3, 10, QLatin1Char('0'));
}

QByteArray numberedOutput(const QMap<int, QString> &matches)
{
    QByteArray output;

    for (int row = 0; row < 80; ++row) {
        output += rowLabel(row).toUtf8() + ' ' + matches.value(row, QStringLiteral("ordinary")).toUtf8() + "\r\n";
    }

    return output;
}
}

class TerminalSearchTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void matchCountLifecycle();
    void navigationWrapsInBothDirections();
    void initialBackwardSelection();
    void searchCompletesDuringContinuousOutput();
    void navigationSurvivesOutput();
    void implicitScrollingNavigatesBothDirections();
    void liveSearchUpdatesPreserveManualViewport();
    void reflowKeepsValidSelection();
    void prunedHistoryDropsOldMatches();
    void resetAndAlternateScreen();
    void querySessionReplacementAndCancellation();
};

// Search counts follow asynchronous completion, new output, clearing and session detachment.
void TerminalSearchTest::matchCountLifecycle()
{
    Session session;
    session.feed("alpha alpha");
    TerminalSearch search;
    QCOMPARE(search.matchCount(), -1);
    search.setSession(&session);
    QCOMPARE(search.matchCount(), -1);
    search.find(QStringLiteral("alpha"));
    QVERIFY(search.isBusy());
    QCOMPARE(search.matchCount(), -1);
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(search.matchCount(), 2);
    search.find(QStringLiteral("missing"));
    QCOMPARE(search.matchCount(), 2);
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(search.matchCount(), 0);
    session.feed(" missing");
    QTRY_COMPARE(search.matchCount(), 1);
    search.clear();
    QCOMPARE(search.matchCount(), -1);
    search.find(QStringLiteral("alpha"));
    QTRY_VERIFY(!search.isBusy());
    search.setSession(nullptr);
    QCOMPARE(search.matchCount(), -1);
}

// Repeated searches move between matches in both directions and wrap at either end.
void TerminalSearchTest::navigationWrapsInBothDirections()
{
    Session session;
    session.resize(40, 8);
    session.feed("alpha\r\nALPHA\r\nAlpha\r\n");
    TerminalSearch search;
    search.setSession(&session);
    search.find(QStringLiteral("alpha"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(search.matchCount(), 3);
    QCOMPARE(session.selectionText(), QStringLiteral("Alpha"));
    search.find(QStringLiteral("alpha"));
    QTRY_COMPARE(session.selectionText(), QStringLiteral("ALPHA"));
    QCOMPARE(search.matchCount(), 3);
    search.find(QStringLiteral("alpha"));
    QTRY_COMPARE(session.selectionText(), QStringLiteral("alpha"));
    QCOMPARE(search.matchCount(), 3);
    search.find(QStringLiteral("alpha"));
    QTRY_COMPARE(session.selectionText(), QStringLiteral("Alpha"));
    QCOMPARE(search.matchCount(), 3);
    search.find(QStringLiteral("alpha"), false);
    QTRY_COMPARE(session.selectionText(), QStringLiteral("alpha"));
    QCOMPARE(search.matchCount(), 3);
}

// An initial backward search selects the oldest match before advancing to the next one.
void TerminalSearchTest::initialBackwardSelection()
{
    Session session;
    session.feed("first marker\r\nsecond MARKER\r\nthird Marker\r\n");
    TerminalSearch search;
    search.setSession(&session);
    search.find(QStringLiteral("marker"), false);
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("marker"));
    QCOMPARE(search.matchCount(), 3);
    search.find(QStringLiteral("marker"), false);
    QTRY_COMPARE(session.selectionText(), QStringLiteral("MARKER"));
    QCOMPARE(search.matchCount(), 3);
}

// Search finishes and preserves its selected match while terminal output continues to arrive.
void TerminalSearchTest::searchCompletesDuringContinuousOutput()
{
    Session session;
    session.resize(80, 5);
    session.feed("needle first\r\nneedle second\r\n");
    QTimer producer;
    const auto appendOutput = [&] {
        session.feed("ordinary log output\r\n");
    };
    connect(&producer, &QTimer::timeout, &session, appendOutput);
    QSignalSpy produced{&producer, &QTimer::timeout};
    producer.start(std::chrono::milliseconds{10});
    QTRY_VERIFY(!produced.isEmpty());
    session.terminal()->scrollTo(0);

    TerminalSearch search;
    search.setSession(&session);
    QSignalSpy applied{&search, &TerminalSearch::matchApplied};
    search.find(QStringLiteral("needle"));
    // Ordinary output can arrive after Find but before its queued work starts.
    appendOutput();
    QTRY_VERIFY(!applied.isEmpty() && !search.isBusy());
    QVERIFY(producer.isActive());
    QCOMPARE(session.selectionText(), QStringLiteral("needle"));
    QVERIFY(produced.wait());
    QVERIFY(producer.isActive());
    QCOMPARE(session.selectionText(), QStringLiteral("needle"));
}

// New output and title updates preserve the current position in search navigation.
void TerminalSearchTest::navigationSurvivesOutput()
{
    Session session;
    session.resize(40, 10);
    session.feed("marker\r\nMARKER\r\nMarker\r\n");
    TerminalSearch search;
    search.setSession(&session);
    search.find(QStringLiteral("marker"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("Marker"));
    session.feed("ordinary output\r\n\x1b]2;new title\x07\x1b[0m");
    search.find(QStringLiteral("marker"));
    QTRY_COMPARE(session.selectionText(), QStringLiteral("MARKER"));
    session.feed("more output\r\n");
    search.find(QStringLiteral("marker"));
    QTRY_COMPARE(session.selectionText(), QStringLiteral("marker"));
    search.find(QStringLiteral("marker"), false);
    QTRY_COMPARE(session.selectionText(), QStringLiteral("MARKER"));
}

// Navigating search results scrolls each selected match into view in either direction.
void TerminalSearchTest::implicitScrollingNavigatesBothDirections()
{
    ViewportFixture fixture;
    QVERIFY(fixture.prepare());
    fixture.session.feed(numberedOutput({{5, QStringLiteral("marker")}, {35, QStringLiteral("MARKER")}, {65, QStringLiteral("Marker")}}));
    QTRY_VERIFY(fixture.visibleText().contains(rowLabel(79)));
    QVERIFY(!fixture.visibleText().contains(rowLabel(5)));
    QVERIFY(fixture.view.scrollSize() < 1);

    struct Navigation {
        bool forward{false};
        int row{0};
        QString text;
    };
    const QList<Navigation> navigation{
        {true, 65, QStringLiteral("Marker")},
        {true, 35, QStringLiteral("MARKER")},
        {true, 5, QStringLiteral("marker")},
        {true, 65, QStringLiteral("Marker")},
        {false, 5, QStringLiteral("marker")},
        {false, 35, QStringLiteral("MARKER")},
        {false, 65, QStringLiteral("Marker")},
        {false, 5, QStringLiteral("marker")},
    };
    int previousRow{79};

    for (const Navigation &step : navigation) {
        const qreal previousPosition{fixture.view.scrollPosition()};
        const bool wasVisible{fixture.visibleText().contains(rowLabel(step.row) + QLatin1Char(' ') + step.text)};
        fixture.view.find(QStringLiteral("marker"), step.forward);
        QTRY_VERIFY(!fixture.view.isSearchBusy());
        QCOMPARE(fixture.view.selectedText(), step.text);
        QTRY_VERIFY(fixture.visibleText().contains(rowLabel(step.row) + QLatin1Char(' ') + step.text));
        QTRY_COMPARE(fixture.visibleSelection(), step.text);

        if (!wasVisible) {
            if (step.row < previousRow) {
                QVERIFY(fixture.view.scrollPosition() < previousPosition);
            } else {
                QVERIFY(fixture.view.scrollPosition() > previousPosition);
            }
        }

        previousRow = step.row;
    }
}

// New matches update the result count while preserving the user's manual scroll position and selection.
void TerminalSearchTest::liveSearchUpdatesPreserveManualViewport()
{
    ViewportFixture fixture;
    QVERIFY(fixture.prepare());
    fixture.session.feed(numberedOutput({{5, QStringLiteral("marker")}}));
    QTRY_VERIFY(fixture.visibleText().contains(rowLabel(79)));
    fixture.view.find(QStringLiteral("marker"));
    QTRY_VERIFY(!fixture.view.isSearchBusy());
    QTRY_VERIFY(fixture.visibleText().contains(QStringLiteral("row-005 marker")));

    fixture.view.scrollTo(0.5);
    QTRY_VERIFY(!fixture.visibleText().contains(rowLabel(5)) && !fixture.visibleText().contains(rowLabel(79)));
    const QString visible{fixture.visibleText()};
    const qint64 offset{fixture.viewportOffset()};
    const qreal size{fixture.view.scrollSize()};
    fixture.session.feed("MARKER appended\r\n");
    QTRY_COMPARE(fixture.view.searchMatchCount(), 2);
    QTRY_VERIFY(!fixture.view.isSearchBusy());
    QTRY_VERIFY(fixture.view.scrollSize() < size);
    QCOMPARE(fixture.visibleText(), visible);
    QCOMPARE(fixture.viewportOffset(), offset);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("marker"));
}

// Searching after reflow still selects the complete Unicode match.
void TerminalSearchTest::reflowKeepsValidSelection()
{
    Session session;
    session.resize(30, 6);
    session.feed(QStringLiteral("prefix 界é needle suffix\r\nother text").toUtf8());
    TerminalSearch search;
    search.setSession(&session);
    const QString query{QStringLiteral("界é needle")};
    search.find(query);
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), query);
    session.resize(7, 8);
    search.find(query);
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), query);
    QCOMPARE(search.matchCount(), 1);
}

// Search drops matches from pruned history and selects the surviving result.
void TerminalSearchTest::prunedHistoryDropsOldMatches()
{
    Session session;
    session.resize(40, 3);
    session.setScrollbackLines(1);
    session.feed("MARKER old\r\n");
    TerminalSearch search;
    search.setSession(&session);
    search.find(QStringLiteral("marker"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("MARKER"));
    session.feed(QByteArray("ordinary output\r\n").repeated(2000) + "marker retained\r\n");
    QVERIFY(!session.historyText().contains(QStringLiteral("MARKER old")));
    search.find(QStringLiteral("marker"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(search.matchCount(), 1);
    QCOMPARE(session.selectionText(), QStringLiteral("marker"));
}

// Search follows the active screen and clears obsolete results after a terminal reset.
void TerminalSearchTest::resetAndAlternateScreen()
{
    Session session;
    session.feed("primary marker");
    TerminalSearch search;
    search.setSession(&session);
    search.find(QStringLiteral("primary"));
    session.feed("\x1b[?1049h\x1b[2J\x1b[Halternate marker");
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(search.matchCount(), 0);
    QVERIFY(session.selectionText().isEmpty());
    search.find(QStringLiteral("alternate"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("alternate"));
    session.feed("\x1b[?1049l");
    search.find(QStringLiteral("primary"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("primary"));
    session.resetTerminal();
    QTRY_COMPARE(search.matchCount(), 0);
    QVERIFY(!search.isBusy());
    QVERIFY(session.selectionText().isEmpty());
    session.feed("new primary");
    search.find(QStringLiteral("primary"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(session.selectionText(), QStringLiteral("primary"));
}

// Replacing queries or sessions and clearing a search prevent stale queued results from being applied.
void TerminalSearchTest::querySessionReplacementAndCancellation()
{
    Session first;
    Session second;
    first.feed(QByteArray("first output\r\n").repeated(1000));
    second.feed("second");
    TerminalSearch search;
    search.setSession(&first);
    search.find(QStringLiteral("first"));
    search.find(QStringLiteral("missing"));
    search.setSession(&second);
    search.find(QStringLiteral("second"));
    QTRY_VERIFY(!search.isBusy());
    QCOMPARE(second.selectionText(), QStringLiteral("second"));
    QVERIFY(first.selectionText().isEmpty());
    search.find(QStringLiteral("pending"));
    search.clear();
    QVERIFY(!search.isBusy());
    QVERIFY(search.status().isEmpty());
    QCoreApplication::processEvents();
    QVERIFY(!search.isBusy());
    QVERIFY(search.status().isEmpty());
    search.find(QStringLiteral("second"));
    search.setSession(nullptr);
    QVERIFY(!search.isBusy());
    QVERIFY(search.status().isEmpty());
    QCoreApplication::processEvents();
    QVERIFY(search.status().isEmpty());
}

int main(int argc, char **argv)
{
    QTemporaryDir config;

    if (!config.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("kwi");
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
    TerminalSearchTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "terminalsearchtest.moc"
