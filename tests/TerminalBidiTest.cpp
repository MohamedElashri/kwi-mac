// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include <BidiLayout.h>
#include <GridShaper_p.h>
#include <PreeditLayout.h>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QTest>
#include <QTextLayout>
#include <Search.h>
#include <Terminal.h>
#include <algorithm>
#include <cmath>

using namespace GhosttyQt;
using namespace GhosttyQt::Quick;

class TerminalBidiTest : public QObject
{
    Q_OBJECT

    static QString rowText(const QVector<Cell> &row)
    {
        QString text;
        for (const auto &cell : row)
            text += cell.text;
        return text;
    }

    static QString glyphKey(const QRawFont &font, quint32 index)
    {
        return font.familyName() + QLatin1Char(':') + QString::number(index);
    }

private Q_SLOTS:
    void languages_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("bold-arabic") << QStringLiteral("\033[1mسلام\033[0m");
        QTest::newRow("arabic") << QString::fromUtf8("سلام بالعالم");
        QTest::newRow("prefixed-arabic") << QString::fromUtf8("Arabic:  السلام عليكم — السَّلَامُ عَلَيْكُمْ");
        QTest::newRow("vocalized-arabic") << QString::fromUtf8("السَّلَامُ عَلَيْكُمْ");
        QTest::newRow("persian") << QString::fromUtf8("فارسی می‌نویسم؛ یک کتاب ۱۲۳");
        QTest::newRow("supplementary-arabic") << QStringLiteral("\U0001ee00\U0001ee01\U0001ee02");
        QTest::newRow("hebrew") << QString::fromUtf8("שָׁלוֹם עוֹלָם");
        QTest::newRow("mixed") << QString::fromUtf8("abc مرحبا 123 (test) שלום ٤٥٦");
        QTest::newRow("wide") << QString::fromUtf8("مرحبا 😀 中文 שלום");
        QTest::newRow("isolate") << QString::fromUtf8("\u2067العربية 123 (abc)\u2069");
        QTest::newRow("embedding") << QString::fromUtf8("a \u202bשלום 123\u202c z");
        QTest::newRow("leading-mark") << QString::fromUtf8("\u200f(שלום) 123");
        QTest::newRow("styled-leading-isolate") << QString::fromUtf8("\u2067\033[31mسلام\033[0m\u2069");
    }

    void languages()
    {
        QFETCH(QString, text);
        Terminal terminal;
        QVERIFY(terminal.resize(80, 4));
        // Exercise decoder and parser boundaries, including split UTF-8 controls.
        const auto bytes = text.toUtf8();
        for (const char c : bytes)
            QVERIFY(terminal.feed(QByteArrayView(&c, 1)));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QString plain = text;
        plain.remove(QStringLiteral("\033[31m"));
        plain.remove(QStringLiteral("\033[1m"));
        plain.remove(QStringLiteral("\033[0m"));
        QCOMPARE(rowText(frame.rows[0]), plain);
        BidiLayout bidi;
        bidi.update(frame);
        QVERIFY(!bidi.rows()[0].logicalCells.isEmpty());
        const auto visual = bidi.visualFrame(frame);
        for (int x = 0; x < frame.columns; ++x) {
            const QPoint logical(x, 0);
            QCOMPARE(bidi.logicalCell(bidi.visualCell(logical)), logical);
            QCOMPARE(visual.rows[0][bidi.visualCell(logical).x()], frame.rows[0][x]);
        }
        QVERIFY(terminal.select({0, 0}, {79, 0}));
        QCOMPARE(terminal.selectionText(), plain);
        const QRegion region(QRect(0, 0, 3, 1));
        QCOMPARE(bidi.visualRegion(region).boundingRect().height(), 1);
        for (int x = 0; x < 3; ++x)
            QVERIFY(bidi.visualRegion(region).contains(bidi.visualCell({x, 0})));
        // Reuse cached permutations after a color/selection-only change.
        Frame selected;
        QVERIFY(terminal.snapshot(selected));
        bidi.update(selected);
        QVERIFY(bidi.visualFrame(selected).rows[0][bidi.visualCell({0, 0}).x()].flags & Selected);
    }

    void batchedDirectionControls_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("ascii-prefix") << QStringLiteral("\u2067ASCII text 123\u2069");
        QTest::newRow("wide-prefix") << QStringLiteral("\u2067中文 😀 ASCII\u2069");
        QTest::newRow("space-prefix") << QStringLiteral("\u200f  hello");
        QTest::newRow("mixed") << QStringLiteral("abc \u2067العربية 123\u2069 end");
    }

    void batchedDirectionControls()
    {
        QFETCH(QString, text);
        Terminal terminal;
        QVERIFY(terminal.resize(80, 4));
        // A single write takes the batched ASCII/Unicode printing paths.
        QVERIFY(terminal.feed(text.toUtf8()));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(rowText(frame.rows[0]), text);
        QCOMPARE(terminal.historyText(), text);
        QVERIFY(terminal.select({0, 0}, {79, 0}));
        QCOMPARE(terminal.selectionText(), text);
        QVERIFY(terminal.resize(7, 8));
        QCOMPARE(terminal.historyText(), text);
    }

    void wideWrapSpacersDoNotAddLogicalText()
    {
        Terminal terminal;
        QVERIFY(terminal.resize(5, 2));
        const QString text = QStringLiteral("שלום界");
        QVERIFY(terminal.feed(text.toUtf8()));
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QCOMPARE(frame.rows[0].last().width, quint8(0));
        QCOMPARE(terminal.historyText(), text);
        BidiLayout bidi;
        bidi.update(frame);
        QVERIFY(!bidi.rows()[0].logicalCells.isEmpty());
        QCOMPARE(rowText(bidi.visualFrame(frame).rows[0]), QStringLiteral("םולש"));
        for (int y = 0; y < frame.rows.size(); ++y)
            for (int x = 0; x < frame.columns; ++x)
                QCOMPARE(bidi.logicalCell(bidi.visualCell({x, y})), QPoint(x, y));
    }

    void fallbackGlyphsKeepLogicalColumns()
    {
        Terminal terminal;
        terminal.resize(30, 2);
        terminal.feed(QStringLiteral("A日B本CλD 中E").toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const auto metrics = fontMetrics(QFont(QStringLiteral("Menlo")), 2, 96);
        GridShaper shaper;
        shaper.setMetrics(metrics, 2);
        const auto glyphs = shaper.shape(frame.rows[0], 0, 13);
        for (const auto &[character, column] :
             QList<QPair<QString, int>>{{QStringLiteral("日"), 1}, {QStringLiteral("本"), 4}, {QStringLiteral("λ"), 7}, {QStringLiteral("中"), 10}}) {
            bool found = false;
            for (const auto &glyph : glyphs) {
                if (!glyph.font.supportsCharacter(character[0]))
                    continue;
                const auto indexes = glyph.font.glyphIndexesForString(character);
                if (!indexes.isEmpty() && glyph.index == indexes[0]) {
                    QCOMPARE(glyph.firstColumn, column);
                    found = true;
                }
            }
            QVERIFY2(found, qPrintable(character));
        }
    }

    void visualOrder()
    {
        Terminal terminal;
        terminal.resize(20, 2);
        terminal.feed(QString::fromUtf8("abc שלום 123").toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        BidiLayout bidi;
        bidi.update(frame);
        QCOMPARE(rowText(bidi.visualFrame(frame).rows[0]), QString::fromUtf8("abc 123 םולש"));
        QCOMPARE(bidi.visualCell({4, 0}), QPoint(11, 0));
        QCOMPARE(bidi.logicalCell({11, 0}), QPoint(4, 0));
        bidi.update(frame, false);
        QCOMPARE(bidi.visualCell({4, 0}), QPoint(4, 0));
        QCOMPARE(rowText(bidi.visualFrame(frame).rows[0]), rowText(frame.rows[0]));
    }

    void shaping_data()
    {
        languages_data();
    }
    void shaping()
    {
        QFETCH(QString, text);

        Terminal terminal;
        terminal.resize(80, 2);
        terminal.feed(text.toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        BidiLayout bidi;
        bidi.update(frame);
        const QFont font(QStringLiteral("Menlo"));
        const auto metrics = fontMetrics(font, 2, 96);
        GridShaper shaper;
        shaper.setMetrics(metrics, 2);
        bool miss = false;
        const auto shaped = shaper.shapeBidi(bidi.rows()[0], &miss);
        QVERIFY(miss);
        QVERIFY(!shaped.isEmpty());
        QStringList actual;
        for (const auto &glyph : shaped) {
            QVERIFY(!glyph.spans.isEmpty());
            for (const auto &span : glyph.spans) {
                QVERIFY(span.sourceRight > span.sourceLeft);
                QVERIFY(span.columnCount >= 1);
                QVERIFY(span.firstColumn >= glyph.firstColumn && span.firstColumn + span.columnCount <= glyph.firstColumn + glyph.columnCount);
            }
            for (int index = 0; index + 1 < glyph.spans.size(); ++index) {
                const auto &left = glyph.spans[index];
                const auto &right = glyph.spans[index + 1];
                QCOMPARE(left.firstColumn + left.columnCount, right.firstColumn);
                QVERIFY2(qAbs(left.sourceRight - right.sourceLeft) < 0.1,
                         qPrintable(QStringLiteral("run %1+%2: cell %3 [%4,%5] next %6 [%7,%8]")
                                        .arg(glyph.firstColumn)
                                        .arg(glyph.columnCount)
                                        .arg(left.firstColumn)
                                        .arg(left.sourceLeft)
                                        .arg(left.sourceRight)
                                        .arg(right.firstColumn)
                                        .arg(right.sourceLeft)
                                        .arg(right.sourceRight)));
            }
            QVERIFY(std::isfinite(glyph.baseline.x()) && std::isfinite(glyph.baseline.y()));
            if (!glyph.font.boundingRect(glyph.index).isEmpty())
                actual += glyphKey(glyph.font, glyph.index);
        }
        QString referenceText = text;
        referenceText.remove(QStringLiteral("\033[31m"));
        referenceText.remove(QStringLiteral("\033[1m"));
        referenceText.remove(QStringLiteral("\033[0m"));
        QFont referenceFont = metrics.pixelFont;
        if (text.contains(QStringLiteral("\033[1m"))) {
            referenceFont.setStyleName({});
            referenceFont.setWeight(QFont::Bold);
        }
        QTextLayout reference(referenceText, referenceFont);
        QTextOption option;
        option.setTextDirection(Qt::LeftToRight);
        option.setWrapMode(QTextOption::NoWrap);
        reference.setTextOption(option);
        reference.beginLayout();
        reference.createLine().setLineWidth(1e9);
        reference.endLayout();
        QStringList expected;
        for (const auto &run : reference.glyphRuns())
            for (const quint32 index : run.glyphIndexes())
                if (!run.rawFont().boundingRect(index).isEmpty())
                    expected += glyphKey(run.rawFont(), index);
        actual.sort();
        expected.sort();
        QCOMPARE(actual, expected); // Includes joining, lam-alef, marks, mirroring and fallback.
        const auto cached = shaper.shapeBidi(bidi.rows()[0], &miss);
        QCOMPARE(cached.size(), shaped.size());
        QVERIFY(!miss);
    }

    void wrappedParagraphsAndHistory()
    {
        Terminal terminal;
        terminal.resize(12, 3);
        const QString text = QString::fromUtf8("abc \u2067العربية 123 (test) فارسی שלום\u2069 end");
        terminal.feed(text.toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        BidiLayout bidi;
        bidi.update(frame);
        for (int y = 0; y < frame.rows.size(); ++y)
            for (int x = 0; x < frame.columns; ++x)
                QCOMPARE(bidi.logicalCell(bidi.visualCell({x, y})), QPoint(x, y));
        QVERIFY(!frame.bidiPrefix.isEmpty());
        terminal.selectAll();
        QCOMPARE(terminal.selectionText(), text);
        terminal.scrollTo(0);
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(!frame.bidiSuffix.isEmpty());
        terminal.resize(20, 4);
        terminal.selectAll();
        QCOMPARE(terminal.selectionText(), text);
    }

    void discontiguousRectangularSelection()
    {
        Terminal terminal;
        terminal.resize(20, 3);
        terminal.feed(QStringLiteral("abc שלום 123\r\nabc שלום 123").toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        BidiLayout bidi;
        bidi.update(frame);
        QRegion logical;
        for (int y = 0; y < 2; ++y)
            for (int x = 6; x <= 9; ++x)
                logical += QRect(bidi.logicalCell({x, y}), QSize(1, 1));
        QVERIFY(terminal.selectCells(logical));
        QVERIFY(terminal.hasSelection());
        QCOMPARE(terminal.selectionText(), QStringLiteral("ום 3\nום 3"));
        QVERIFY(terminal.snapshot(frame));
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 20; ++x)
                QCOMPARE(bool(frame.rows[y][x].flags & Selected), logical.contains({x, y}));
        terminal.resize(24, 4);
        QCOMPARE(terminal.selectionText(), QStringLiteral("ום 3\nום 3"));
        terminal.clearSelection();
        QVERIFY(!terminal.hasSelection());
        QVERIFY(terminal.snapshot(frame));
        for (const auto &row : frame.rows)
            for (const auto &cell : row)
                QVERIFY(!(cell.flags & Selected));
    }

    void rectangleExtendsAcrossScrollback()
    {
        Terminal terminal;
        terminal.resize(20, 3);
        for (int line = 0; line < 8; ++line)
            terminal.feed(QStringLiteral("שלום עולם\r\n").toUtf8());
        terminal.scrollTo(0);
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(terminal.selectCells(QRegion(QRect(0, 0, 2, 3))));
        terminal.scrollTo(2);
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(terminal.selectCells(QRegion(QRect(0, 0, 2, 3)), 0, 4));
        QCOMPARE(terminal.selectionText(), QStringLiteral("של\nשל\nשל\nשל\nשל"));
        QVERIFY(terminal.selectCells(QRegion(QRect(0, 0, 2, 2)), 1, 3));
        QCOMPARE(terminal.selectionText(), QStringLiteral("של\nשל\nשל"));
        terminal.feed("\033[?1049h");
        QVERIFY(!terminal.hasSelection());
    }

    void joiningAcrossSoftWrap()
    {
        Terminal terminal;
        terminal.resize(4, 3);
        terminal.feed(QStringLiteral("بببببببب").toUtf8());
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        QVERIFY(frame.rowWrapped[0]);
        BidiLayout bidi;
        bidi.update(frame);
        QVERIFY(bidi.rows()[0].joinAfter);
        QVERIFY(bidi.rows()[1].joinBefore);
        QVERIFY(!bidi.rows()[0].joinBefore);
        QVERIFY(!bidi.rows()[1].joinAfter);
        GridShaper shaper;
        QFont font(QStringLiteral("Menlo"));
        font.setPointSizeF(14);
        shaper.setMetrics(fontMetrics(font, 2, 72), 2);
        QVERIFY(!shaper.shapeBidi(bidi.rows()[0]).isEmpty());
        QVERIFY(!shaper.shapeBidi(bidi.rows()[1]).isEmpty());
    }

    void searchUsesLogicalArabic()
    {
        Terminal terminal;
        terminal.resize(15, 4);
        terminal.feed(QStringLiteral("\u2067سلام بالعالم\u2069\r\nفارسی می‌نویسم\r\nשלום עולם").toUtf8());
        for (const QString &needle : {QStringLiteral("سلام"), QStringLiteral("می‌نویسم"), QStringLiteral("שלום")}) {
            auto search = terminal.createSearch();
            QVERIFY(search->setNeedle(needle));
            QVERIFY(search->feed() != Search::Status::Error);
            Search::Status status;
            do {
                status = search->tick();
                if (status == Search::Status::FeedRequired)
                    status = search->feed();
            } while (status == Search::Status::Running || status == Search::Status::FeedRequired);
            QCOMPARE(status, Search::Status::Complete);
            QCOMPARE(search->results().totalMatches, qsizetype(1));
            QVERIFY(search->select(true));
            QVERIFY(search->applySelection());
            QVERIFY(terminal.selectionText().contains(needle));
        }
    }

    void asciiFastPath()
    {
        Terminal terminal;
        terminal.resize(80, 4);
        terminal.feed("user@mac:~$ git status\r\nOn branch main\r\n");
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        BidiLayout bidi;
        bidi.update(frame);
        for (const auto &row : bidi.rows())
            QVERIFY(row.logicalToVisual.isEmpty());
        QCOMPARE(bidi.visualFrame(frame).rows, frame.rows);
    }

    void preeditUsesParagraphContext()
    {
        Terminal terminal;
        terminal.resize(30, 3);
        terminal.feed("prompt> ");
        Frame frame;
        QVERIFY(terminal.snapshot(frame));
        const auto preedit = layoutPreedit(frame, QString::fromUtf8("سلام"), {});
        const auto composed = frameWithPreedit(frame, preedit);
        BidiLayout bidi;
        bidi.update(composed);
        QVERIFY(!bidi.rows()[0].logicalCells.isEmpty());
        QCOMPARE(rowText(frame.rows[0]), QStringLiteral("prompt> "));
        QCOMPARE(rowText(composed.rows[0]), QString::fromUtf8("prompt> سلام"));
        const QPoint visual = bidi.visualCell({8, 0});
        QCOMPARE(bidi.logicalCell(visual), QPoint(8, 0));
        QCOMPARE(composed.cursorStyle, CursorStyle::Beam);
    }
};

QTEST_MAIN(TerminalBidiTest)
#include "TerminalBidiTest.moc"
