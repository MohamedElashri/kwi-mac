// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Frame.h"
#include "Session.h"
#include "Terminal.h"
#include "TerminalView.h"

#include <KLocalizedString>
#include <QAccessible>
#include <QAccessibleEvent>
#include <QAccessibleTextInterface>
#include <QAccessibleTextUpdateEvent>
#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QLatin1String>
#include <QObject>
#include <QPointF>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>

using namespace Kwi;

namespace
{
class Fixture
{
public:
    Fixture()
        : view(window.contentItem())
    {
        window.resize(640, 400);
        view.setSize(QSizeF(640, 400));
        view.setSession(&session);
    }

    bool prepare(int columns, int rows)
    {
        window.show();

        if (!QTest::qWaitForWindowExposed(&window)) {
            return false;
        }

        padding = view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().topLeft();

        return resize(columns, rows);
    }

    bool resize(int columns, int rows)
    {
        const QSizeF cell = view.inputMethodQuery(Qt::ImCursorRectangle).toRectF().size();
        view.setSize(QSizeF(columns * cell.width() + 2 * padding.x(), rows * cell.height() + 2 * padding.y()));

        return QTest::qWaitFor([&] {
            GhosttyQt::Frame frame;

            return session.snapshot(frame) && frame.columns == columns && frame.rows.size() == rows;
        });
    }

    QAccessibleTextInterface *text()
    {
        const auto accessible = QAccessible::queryAccessibleInterface(&view);

        return accessible ? accessible->textInterface() : nullptr;
    }

    Session session;
    QQuickWindow window;
    TerminalView view;
    QPointF padding;
};

class TextUpdates
{
    Q_DISABLE_COPY_MOVE(TextUpdates)

public:
    explicit TextUpdates(QObject *object)
        : m_object(object)
        , m_wasActive(QAccessible::isActive())
        , m_previousHandler(QAccessible::installUpdateHandler(record))
    {
        instance = this;
        QAccessible::setActive(true);
    }

    ~TextUpdates()
    {
        QAccessible::installUpdateHandler(m_previousHandler);
        QAccessible::setActive(m_wasActive);
        instance = nullptr;
    }

    QString current;
    int changes{0};
    bool valid{true};

private:
    static void record(QAccessibleEvent *event)
    {
        if (!instance || event->object() != instance->m_object || event->type() != QAccessible::TextUpdated) {
            return;
        }

        const auto *update = static_cast<QAccessibleTextUpdateEvent *>(event);
        const int position{update->changePosition()};
        const auto removed = update->textRemoved();
        const auto inserted = update->textInserted();
        instance->valid = instance->valid && position >= 0 && position <= instance->current.size() && removed.isValidUtf16() && inserted.isValidUtf16()
            && instance->current.mid(position, removed.size()) == removed;
        instance->current.replace(position, removed.size(), inserted);
        ++instance->changes;
    }

    inline static TextUpdates *instance{nullptr};
    QObject *m_object;
    bool m_wasActive;
    QAccessible::UpdateHandler m_previousHandler;
};
}

class AccessibilityTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void logicalText_data();
    void logicalText();
    void unicodeSelectionAndGeometryAcrossWrap();
    void rectangularSelection();
    void textAttributes();
    void logicalTextSurvivesReflow();
    void updatesDescribeLogicalTextChanges();
    void sessionAndScreenChanges();
};

void AccessibilityTest::logicalText_data()
{
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("rows");
    QTest::addColumn<QByteArray>("output");
    QTest::addColumn<QString>("expected");

    QTest::newRow("soft-wrap") << 8 << 3 << QByteArray("abcdefghij\r\nnext") << QStringLiteral("abcdefghij\nnext");
    QTest::newRow("hard-lines") << 8 << 3 << QByteArray("one\r\ntwo\r\n") << QStringLiteral("one\ntwo\n");
    QTest::newRow("written-spaces") << 8 << 2 << QByteArray("A  \r\nB") << QStringLiteral("A  \nB");
    QTest::newRow("interior-gaps") << 8 << 2 << QByteArray("\x1b[3CX\r\nY") << QStringLiteral("   X\nY");
    QTest::newRow("erased-padding") << 8 << 2 << QByteArray("abcdef\x1b[3G\x1b[K\r\nZ") << QStringLiteral("ab\nZ");
    QTest::newRow("wrapped-spaces") << 8 << 2 << QByteArray("ABCDEF  Z") << QStringLiteral("ABCDEF  Z");
    QTest::newRow("wide-wrap-spacer") << 5 << 2 << QStringLiteral("abcd界").toUtf8() << QStringLiteral("abcd界");
    QTest::newRow("empty-lines") << 8 << 2 << QByteArray() << QStringLiteral("\n");
    QTest::newRow("hard-break-at-margin") << 8 << 2 << QByteArray("12345678\r\nA") << QStringLiteral("12345678\nA");
}

// Accessible text joins soft wraps and preserves hard breaks and written spaces.
void AccessibilityTest::logicalText()
{
    QFETCH(int, columns);
    QFETCH(int, rows);
    QFETCH(QByteArray, output);
    QFETCH(QString, expected);
    Fixture fixture;
    QVERIFY(fixture.prepare(columns, rows));
    auto *text = fixture.text();
    QVERIFY(text);
    fixture.session.feed(output);
    QTRY_COMPARE(text->text(0, -1), expected);
    QCOMPARE(text->characterCount(), expected.size());
}

// Accessible offsets and selections map Unicode characters to their terminal cells across wrapped lines.
void AccessibilityTest::unicodeSelectionAndGeometryAcrossWrap()
{
    Fixture fixture;
    QVERIFY(fixture.prepare(6, 3));
    auto *text = fixture.text();
    QVERIFY(text);
    fixture.session.feed(QStringLiteral("A界é🙂XYZ").toUtf8());
    QTRY_COMPARE(text->text(0, -1), QStringLiteral("A界é🙂XYZ\n"));

    const QRect latin{text->characterRect(0)};
    const QRect wide{text->characterRect(1)};
    QVERIFY(latin.width() > 0);
    QCOMPARE(wide.width(), 2 * latin.width());
    QCOMPARE(text->characterRect(2), text->characterRect(3));
    QCOMPARE(text->characterRect(4), text->characterRect(5));
    QCOMPARE(text->offsetAtPoint(wide.center()), 1);
    QVERIFY(text->characterRect(6).top() > latin.top());
    QCOMPARE(text->offsetAtPoint(text->characterRect(6).center()), 6);

    text->setSelection(0, 1, 8);
    QCOMPARE(fixture.view.selectedText(), QStringLiteral("界é🙂XY"));
    QVERIFY(fixture.view.hasSelection());
    QTRY_COMPARE(fixture.session.selectionText(), QStringLiteral("界é🙂XY"));
    QTRY_COMPARE(text->selectionCount(), 1);
    int start{-1};
    int end{-1};
    text->selection(0, &start, &end);
    QCOMPARE(start, 1);
    QCOMPARE(end, 8);

    text->setSelection(0, 3, 4);
    QTRY_COMPARE(fixture.session.selectionText(), QStringLiteral("é"));
    QTRY_VERIFY((text->selection(0, &start, &end), start == 2 && end == 4));
    text->setSelection(0, 4, 5);
    QTRY_COMPARE(fixture.session.selectionText(), QStringLiteral("🙂"));
    QTRY_VERIFY((text->selection(0, &start, &end), start == 4 && end == 6));

    fixture.session.terminal()->select({1, 1}, {1, 0});
    QTRY_VERIFY((text->selection(0, &start, &end), start == 1 && end == 8));
    QCOMPARE(text->text(start, end), QStringLiteral("界é🙂XY"));
}

// The accessibility interface exposes rectangular selection bounds and can replace or clear the selection.
void AccessibilityTest::rectangularSelection()
{
    Fixture fixture;
    QVERIFY(fixture.prepare(8, 3));
    auto *text = fixture.text();
    QVERIFY(text);
    fixture.session.feed("abcd\r\nefgh\r\nijkl");
    QTRY_COMPARE(text->text(0, -1), QStringLiteral("abcd\nefgh\nijkl"));
    QVERIFY(fixture.session.terminal()->select({1, 0}, {2, 2}, true));
    QTRY_COMPARE(fixture.view.selectedText(), QStringLiteral("bc\nfg\njk"));
    QTRY_COMPARE(text->selectionCount(), 1);
    int start{-1};
    int end{-1};
    text->selection(0, &start, &end);
    QCOMPARE(start, 1);
    QCOMPARE(end, 13);
    text->setSelection(0, 2, 7);
    QTRY_COMPARE(fixture.view.selectedText(), QStringLiteral("cd\nef"));
    text->removeSelection(0);
    QTRY_COMPARE(text->selectionCount(), 0);
    QVERIFY(!fixture.view.hasSelection());
}

// Accessible text attributes report terminal colors, font settings and character styles.
void AccessibilityTest::textAttributes()
{
    Fixture fixture;
    QVERIFY(fixture.prepare(12, 2));
    fixture.view.setFontFamily(QStringLiteral("monospace"));
    fixture.view.setFontPointSize(12);
    QVERIFY(fixture.resize(12, 2));
    auto *text = fixture.text();
    QVERIFY(text);
    fixture.session.feed("\x1b[1;3;4:2;9;38;2;1;2;3;48;2;4;5;6mX\x1b[0mY");
    QTRY_COMPARE(text->text(0, -1), QStringLiteral("XY\n"));
    int start{-1};
    int end{-1};
    const QString attributes{text->attributes(0, &start, &end)};
    QCOMPARE(start, 0);
    QCOMPARE(end, 1);

    for (const auto &attribute : {"color:rgb(1,2,3);",
                                  "background-color:rgb(4,5,6);",
                                  "font-weight:bold;",
                                  "font-style:italic;",
                                  "text-underline-type:double;",
                                  "text-line-through-type:single;",
                                  "font-family:\"monospace\";",
                                  "font-size:12pt;"}) {
        QVERIFY2(attributes.contains(QLatin1String(attribute)), qPrintable(attributes));
    }

    QVERIFY(!attributes.contains(QStringLiteral("foreground-color")));
    const QString normal{text->attributes(1, &start, &end)};
    QVERIFY(normal.contains(QStringLiteral("text-underline-type:none;")));
    QVERIFY(normal.contains(QStringLiteral("text-line-through-type:none;")));
}

// Accessible text and selection offsets remain consistent when resizing reflows terminal rows.
void AccessibilityTest::logicalTextSurvivesReflow()
{
    Fixture fixture;
    QVERIFY(fixture.prepare(8, 2));
    auto *text = fixture.text();
    QVERIFY(text);
    fixture.session.feed("abcdefghijklmnop");
    QTRY_COMPARE(text->text(0, -1), QStringLiteral("abcdefghijklmnop"));
    const QRect before{text->characterRect(10)};
    QVERIFY(fixture.resize(5, 4));
    QTRY_VERIFY(text->characterRect(10).top() > before.top());
    QCOMPARE(text->text(0, -1), QStringLiteral("abcdefghijklmnop"));
    text->setSelection(0, 3, 13);
    QTRY_COMPARE(fixture.session.selectionText(), QStringLiteral("defghijklm"));
}

// Accessibility text events reconstruct changed content and omit updates caused solely by reflow.
void AccessibilityTest::updatesDescribeLogicalTextChanges()
{
    Fixture fixture;
    TextUpdates updates{&fixture.view};
    QVERIFY(fixture.prepare(6, 2));
    auto *text = fixture.text();
    QVERIFY(text);
    QTRY_COMPARE(updates.current, QStringLiteral("\n"));
    fixture.session.feed("abcd");
    QTRY_COMPARE(updates.current, QStringLiteral("abcd\n"));
    fixture.session.feed("efgh");
    QTRY_COMPARE(updates.current, QStringLiteral("abcdefgh"));
    QCOMPARE(updates.current, text->text(0, -1));
    const int changes{updates.changes};
    const int previousX{text->characterRect(7).x()};
    QVERIFY(fixture.resize(5, 2));
    QTRY_VERIFY(text->characterRect(7).x() != previousX);
    QCOMPARE(text->text(0, -1), QStringLiteral("abcdefgh"));
    QCOMPARE(updates.current, text->text(0, -1));
    QCOMPARE(updates.changes, changes);
    QVERIFY(updates.valid);
}

// Accessible text follows alternate-screen changes and session attachment or detachment.
void AccessibilityTest::sessionAndScreenChanges()
{
    Fixture fixture;
    TextUpdates updates{&fixture.view};
    QVERIFY(fixture.prepare(12, 3));
    fixture.session.feed("first\r\nsecond\r\nthird");
    QTRY_COMPARE(updates.current, QStringLiteral("first\nsecond\nthird"));
    fixture.session.feed("\x1b[?1049h\x1b[Hother");
    QTRY_COMPARE(updates.current, QStringLiteral("other\n\n"));
    fixture.session.feed("\x1b[?1049l");
    QTRY_COMPARE(updates.current, QStringLiteral("first\nsecond\nthird"));
    fixture.view.setSession(nullptr);
    QTRY_COMPARE(updates.current, QString());
    fixture.view.setSession(&fixture.session);
    QVERIFY(fixture.resize(12, 3));
    QTRY_COMPARE(updates.current, QStringLiteral("first\nsecond\nthird"));
    QVERIFY(updates.valid);
}

int main(int argc, char **argv)
{
    QTemporaryDir configuration;

    if (!configuration.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", QFile::encodeName(configuration.path()));
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("kwi");
    AccessibilityTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "accessibilitytest.moc"
