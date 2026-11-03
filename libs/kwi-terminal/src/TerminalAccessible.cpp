// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "Session.h"
#include "TerminalView.h"

#include <KLocalizedString>
#include <QAccessible>
#include <QAccessibleObject>
#include <QKeySequence>
#include <QPair>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QStringList>
#include <QVector>
#include <QWindow>

namespace Kwi
{
class TerminalAccessible final : public QAccessibleObject, public QAccessibleTextInterface, public QAccessibleActionInterface
{
public:
    explicit TerminalAccessible(TerminalView *view)
        : QAccessibleObject{view}
    {
    }

    TerminalView *view() const
    {
        return qobject_cast<TerminalView *>(object());
    }

    QWindow *window() const override
    {
        auto *quick = view() ? view()->window() : nullptr;

        if (quick) {
            if (auto *host = QQuickRenderControl::renderWindowFor(quick)) {
                return host;
            }
        }

        return quick;
    }

    QAccessibleInterface *parent() const override
    {
        if (!view()) {
            return nullptr;
        }

        for (auto *parentItem = view()->parentItem(); parentItem; parentItem = parentItem->parentItem()) {
            if (auto *accessible = QAccessible::queryAccessibleInterface(parentItem)) {
                return accessible;
            }
        }

        return window() ? QAccessible::queryAccessibleInterface(window()) : nullptr;
    }

    QAccessibleInterface *child(int) const override
    {
        return nullptr;
    }

    QAccessibleInterface *childAt(int, int) const override
    {
        return nullptr;
    }

    int childCount() const override
    {
        return 0;
    }

    int indexOfChild(const QAccessibleInterface *) const override
    {
        return -1;
    }

    QAccessibleInterface *focusChild() const override
    {
        return view() && view()->hasActiveFocus() ? const_cast<TerminalAccessible *>(this) : nullptr;
    }

    QAccessible::Role role() const override
    {
#ifdef Q_OS_MACOS
        // Qt's Cocoa bridge maps Terminal to AXGroup and omits its text value.
        // EditableText exposes AXTextArea plus this interface's cursor/selection;
        // state().readOnly still describes the actual terminal input policy.
        return QAccessible::EditableText;
#else
        return QAccessible::Terminal;
#endif
    }

    QAccessible::State state() const override
    {
        QAccessible::State state;

        if (!view()) {
            state.invalid = true;

            return state;
        }

        state.focusable = true;
        state.focused = view()->hasActiveFocus();
        state.invisible = !view()->isVisible();
        state.disabled = !view()->isEnabled();
        state.readOnly = !view()->session() || view()->session()->isReadOnly();
        state.multiLine = true;
        state.selectableText = true;

        return state;
    }

    QRect rect() const override
    {
        if (!view() || !window()) {
            return {};
        }

        return QRectF(view()->mapToGlobal(QPointF()), view()->mapToGlobal(QPointF(view()->width(), view()->height()))).normalized().toAlignedRect();
    }

    QString text(QAccessible::Text type) const override
    {
        if (!view()) {
            return {};
        }

        if (type == QAccessible::Name) {
            auto *context = qmlContext(view());
            const QString name{context && context->isValid() ? QQmlProperty::read(view(), QStringLiteral("Accessible.name"), context).toString() : QString{}};

            if (!name.isEmpty()) {
                return name;
            }

            return view()->session()
                ? i18n("Terminal: %1",
                       (view()->session()->hasApplicationTitle() ? view()->session()->applicationTitle() : view()->session()->launchedProgramName()))
                : i18n("Terminal");
        }

        if (type == QAccessible::Description) {
            auto *context = qmlContext(view());
            const QString description{context && context->isValid() ? QQmlProperty::read(view(), QStringLiteral("Accessible.description"), context).toString()
                                                                    : QString{}};

            if (!description.isEmpty()) {
                return description;
            }

            return i18n("Terminal output. Use keyboard selection mode to review and copy text.");
        }

        if (type == QAccessible::Value) {
            return content().text;
        }

        return {};
    }

    void *interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::TextInterface) {
            return static_cast<QAccessibleTextInterface *>(this);
        }

        if (type == QAccessible::ActionInterface) {
            return static_cast<QAccessibleActionInterface *>(this);
        }

        return nullptr;
    }

    QColor foregroundColor() const override
    {
        return view() ? view()->m_frame.foreground : QColor();
    }

    QColor backgroundColor() const override
    {
        return view() ? view()->m_frame.background : QColor();
    }

    struct Content {
        QString text;
        QVector<QPoint> cells;
        QPair<int, int> selection{-1, -1};
        QPoint end;
    };

    const Content &content() const
    {
        const auto *terminal = view();

        if (terminal && m_contentValid && m_contentRevision == terminal->m_frameRevision) {
            return m_content;
        }

        m_content = {};
        m_contentValid = true;
        m_contentRevision = terminal ? terminal->m_frameRevision : 0;
        Content &result = m_content;

        if (!terminal) {
            return result;
        }

        const auto &frame = terminal->m_frame;
        const auto &rows = frame.rows;
        const auto append = [&result](const QString &text, QPoint cell, bool selected) {
            const int start{int(result.text.size())};
            result.text += text;
            result.cells.resize(result.text.size(), cell);

            if (selected) {
                if (result.selection.first < 0) {
                    result.selection.first = start;
                }

                // A rectangular selection is still one native selection.
                result.selection.second = result.text.size();
            }
        };

        for (int y{0}; y < rows.size(); ++y) {
            const auto &row = rows[y];
            const bool wrapped{frame.rowWrapped.value(y)};
            qsizetype length{row.size()};
            result.end = {0, y};

            // Empty padding is not output. Written spaces and gaps within a
            // logical line remain significant, including across soft wraps.
            while (!wrapped && length > 0 && row[length - 1].text.isEmpty()) {
                --length;
            }

            for (int x{0}; x < length; ++x) {
                const auto &cell = row[x];

                if (cell.width == 0) {
                    continue;
                }

                append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text, {x, y}, cell.flags & GhosttyQt::Selected);
                result.end = {x + cell.width, y};
            }

            if (!wrapped && y + 1 < rows.size()) {
                append(QStringLiteral("\n"),
                       result.end,
                       !row.isEmpty() && !rows[y + 1].isEmpty() && (row.last().flags & GhosttyQt::Selected)
                           && (rows[y + 1].first().flags & GhosttyQt::Selected));
            }
        }

        return result;
    }

    int offsetFor(const Content &textContent, QPoint cell) const
    {
        for (int i{0}; i < textContent.cells.size(); ++i) {
            const QPoint current{textContent.cells[i]};

            if (current.y() > cell.y()) {
                return i > 0 && textContent.text[i - 1] == QLatin1Char('\n') ? i - 1 : i;
            }

            if (current.y() == cell.y() && current.x() >= cell.x()) {
                return i;
            }
        }

        return textContent.text.size();
    }

    QPoint cellForOffset(int offset) const
    {
        QPoint cell{content().cells[offset]};
        // Logical line endings can lie just beyond the last physical cell.
        cell.setX(qMin(cell.x(), view()->m_frame.columns - 1));

        return cell;
    }

    QString text(int start, int end) const override
    {
        const auto value = content().text;

        if (end == -1) {
            end = value.size();
        }

        start = qBound(0, start, int(value.size()));
        end = qBound(start, end, int(value.size()));

        return value.mid(start, end - start);
    }

    int characterCount() const override
    {
        return content().text.size();
    }

    int cursorPosition() const override
    {
        if (!view()) {
            return 0;
        }

#ifdef Q_OS_MACOS
        if (!view()->m_keyboardSelectionMode && m_reviewCursor >= 0 && m_reviewRevision == view()->m_frameRevision)
            return qMin(m_reviewCursor, characterCount());
#endif

        const QPoint cursor{view()->m_keyboardSelectionMode ? view()->m_keyboardCursor : view()->glyphStart(view()->m_frame.cursor)};

        return cursor.y() < 0 ? 0 : offsetFor(content(), cursor);
    }

    void setCursorPosition(int position) override
    {
        if (!view() || !view()->session() || !view()->session()->terminal()) {
            return;
        }

        const auto value = content();

        if (view()->m_frame.rows.isEmpty()) {
            return;
        }

#ifdef Q_OS_MACOS
        if (!view()->m_keyboardSelectionMode) {
            // Cocoa/VoiceOver can set AXSelectedTextRange just to track its
            // review caret. Do not enter the user-invoked selection mode,
            // which consumes typing. New terminal frames expire this offset.
            m_reviewCursor = qBound(0, position, int(value.text.size()));
            m_reviewRevision = view()->m_frameRevision;
            view()->updateAccessibility();
            return;
        }
#endif

        view()->setKeyboardSelectionMode(true);
        position = qBound(0, position, int(value.text.size()));
        // Logical positions include newlines; only physical cursor/hit-test
        // positions are normalized to the beginning of a wide glyph.
        view()->m_keyboardCursor = position == value.text.size() ? value.end : value.cells[position];
        const QPoint cell{view()->glyphStart({qMin(view()->m_keyboardCursor.x(), view()->m_frame.columns - 1), view()->m_keyboardCursor.y()})};
        view()->session()->terminal()->select(cell, cell);

        Q_EMIT view()->keyboardSelectionChanged();
    }

    int selectionCount() const override
    {
        return content().selection.first >= 0 ? 1 : 0;
    }

    void selection(int index, int *start, int *end) const override
    {
        *start = *end = -1;
        const auto value = content();

        if (index == 0) {
            *start = value.selection.first;
            *end = value.selection.second;
        }
    }

    void addSelection(int start, int end) override
    {
        setSelection(0, start, end);
    }

    void setSelection(int index, int start, int end) override
    {
        if (index != 0 || !view() || !view()->session() || !view()->session()->terminal()) {
            return;
        }

        const auto value = content();
        start = qBound(0, start, int(value.cells.size()));
        end = qBound(start, end, int(value.cells.size()));

        if (start == end) {
            removeSelection(0);

            return;
        }

        const QPoint first{cellForOffset(start)};
        const QPoint last{cellForOffset(end - 1)};
        view()->setKeyboardSelectionMode(false);
        view()->session()->terminal()->select(first, last);
    }

    void removeSelection(int index) override
    {
        if (index != 0 || !view() || !view()->session() || !view()->session()->terminal()) {
            return;
        }

        view()->setKeyboardSelectionMode(false);
        view()->session()->terminal()->clearSelection();
    }

    QRect characterRect(int offset) const override
    {
        const auto value = content();

        if (!view() || !window() || offset < 0 || offset >= value.cells.size()) {
            return {};
        }

        const QPoint cell{cellForOffset(offset)};
        const auto local = view()->logicalCellRectangle(cell);
        return QRectF(view()->mapToGlobal(local.topLeft()), view()->mapToGlobal(local.bottomRight())).normalized().toAlignedRect();
    }

    int offsetAtPoint(const QPoint &point) const override
    {
        if (!view() || !rect().contains(point) || view()->m_frame.rows.isEmpty()) {
            return -1;
        }

        const QPoint cell{view()->cellAt(view()->mapFromGlobal(point))};

        if (cell.y() >= view()->m_frame.rows.size() || cell.x() >= view()->m_frame.rows.at(cell.y()).size()) {
            return -1;
        }

        return offsetFor(content(), view()->glyphStart(cell));
    }

    void scrollToSubstring(int start, int end) override
    {
        // All offsets are already in the viewport, so move the review cursor without scrolling.
        if (start >= 0 && end >= start && start < characterCount()) {
            setCursorPosition(start);
        }
    }

    QString attributes(int offset, int *start, int *end) const override
    {
        *start = *end = -1;
        const auto value = content();

        if (!view() || offset < 0 || offset >= value.cells.size()) {
            return {};
        }

        const QPoint cell{value.cells[offset]};
        *start = offset;

        while (*start > 0 && value.cells[*start - 1] == cell) {
            --*start;
        }

        *end = offset + 1;

        while (*end < value.cells.size() && value.cells[*end] == cell) {
            ++*end;
        }

        const QPoint physicalCell{cellForOffset(offset)};
        const auto &native = view()->m_frame.rows.at(physicalCell.y()).at(physicalCell.x());
        const QColor foreground{QColor::fromRgba(native.foreground)};
        const QColor background{QColor::fromRgba(native.background)};
        // Use the same IAccessible2 escaping as Qt's text widgets.
        QString family{view()->m_font.family()};

        for (const QChar character : {u'\\', u':', u',', u'=', u';', u'"'}) {
            family.replace(character, QString{QLatin1Char('\\')} + character);
        }

        return QStringLiteral(
                   "color:rgb(%1,%2,%3);background-color:rgb(%4,%5,%6);font-weight:%7;font-style:%8;"
                   "text-underline-type:%9;text-line-through-type:%10;font-family:\"%11\";font-size:%12pt;")
            .arg(foreground.red())
            .arg(foreground.green())
            .arg(foreground.blue())
            .arg(background.red())
            .arg(background.green())
            .arg(background.blue())
            .arg((native.flags & GhosttyQt::Bold) ? QStringLiteral("bold") : QStringLiteral("normal"),
                 (native.flags & GhosttyQt::Italic) ? QStringLiteral("italic") : QStringLiteral("normal"),
                 native.underline == 2                       ? QStringLiteral("double")
                     : (native.flags & GhosttyQt::Underline) ? QStringLiteral("single")
                                                             : QStringLiteral("none"),
                 (native.flags & GhosttyQt::Strike) ? QStringLiteral("single") : QStringLiteral("none"),
                 family)
            .arg(view()->m_font.pointSizeF());
    }

    QStringList actionNames() const override
    {
        return {QAccessibleActionInterface::setFocusAction(), QAccessibleActionInterface::showMenuAction()};
    }

    void doAction(const QString &name) override
    {
        if (view() && name == QAccessibleActionInterface::setFocusAction()) {
            Q_EMIT view()->focusRequested();

            if (window()) {
                window()->requestActivate();
            }

            view()->forceActiveFocus();
        } else if (view() && name == QAccessibleActionInterface::showMenuAction()) {
            view()->showContextMenu();
        }
    }

    QStringList keyBindingsForAction(const QString &name) const override
    {
        if (name == QAccessibleActionInterface::showMenuAction()) {
            // Hosts can reserve Shift+F10 for their application menu.
            return {QKeySequence{Qt::Key_Menu}.toString(QKeySequence::NativeText)};
        }

        return {};
    }

    void update()
    {
        if (!view()) {
            return;
        }

        const QString current{content().text};

        if (current != m_previousText) {
            int prefix{0};

            while (prefix < current.size() && prefix < m_previousText.size() && current[prefix] == m_previousText[prefix]) {
                ++prefix;
            }

            if (prefix > 0 && prefix < current.size() && current[prefix].isLowSurrogate() && current[prefix - 1].isHighSurrogate()) {
                --prefix;
            }

            int suffix{0};

            while (suffix < current.size() - prefix && suffix < m_previousText.size() - prefix
                   && current[current.size() - suffix - 1] == m_previousText[m_previousText.size() - suffix - 1]) {
                ++suffix;
            }

            const qsizetype suffixStart{current.size() - suffix};

            if (suffixStart > 0 && suffixStart < current.size() && current[suffixStart].isLowSurrogate() && current[suffixStart - 1].isHighSurrogate()) {
                --suffix;
            }

            const QString previous{m_previousText};
            m_previousText = current;
            QAccessibleTextUpdateEvent event(view(),
                                             prefix,
                                             previous.mid(prefix, previous.size() - prefix - suffix),
                                             current.mid(prefix, current.size() - prefix - suffix));
            event.setCursorPosition(cursorPosition());
            QAccessible::updateAccessibility(&event);
        }

        const int cursor{cursorPosition()};

        if (cursor != m_previousCursor) {
            m_previousCursor = cursor;
            QAccessibleTextCursorEvent event(view(), cursor);
            QAccessible::updateAccessibility(&event);
        }

        const auto selectionRange = content().selection;

        if (selectionRange != m_previousSelection) {
            m_previousSelection = selectionRange;
            QAccessibleTextSelectionEvent event(view(), selectionRange.first, selectionRange.second);
            QAccessible::updateAccessibility(&event);
        }

        const QString name{text(QAccessible::Name)};

        if (name != m_previousName) {
            m_previousName = name;
            QAccessibleEvent event(view(), QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
        }

        const auto currentState = state();

        if (m_previousFocused != currentState.focused || m_previousReadOnly != currentState.readOnly) {
            QAccessible::State changed;
            changed.focused = m_previousFocused != currentState.focused;
            changed.readOnly = m_previousReadOnly != currentState.readOnly;
            m_previousFocused = currentState.focused;
            m_previousReadOnly = currentState.readOnly;
            QAccessibleStateChangeEvent event(view(), changed);
            QAccessible::updateAccessibility(&event);

            if (currentState.focused) {
                QAccessibleEvent focus(view(), QAccessible::Focus);
                QAccessible::updateAccessibility(&focus);
            }
        }
    }

private:
#ifdef Q_OS_MACOS
    int m_reviewCursor{-1};
    quint64 m_reviewRevision{0};
#endif
    mutable Content m_content;
    mutable quint64 m_contentRevision{0};
    mutable bool m_contentValid{false};
    QString m_previousText;
    QString m_previousName;
    int m_previousCursor{-1};
    QPair<int, int> m_previousSelection{-1, -1};
    bool m_previousFocused{false};
    bool m_previousReadOnly{false};
};

void TerminalView::installAccessibility()
{
    static const bool installed = [] {
        QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
            if (auto *view = qobject_cast<TerminalView *>(object)) {
                return new TerminalAccessible(view);
            }

            return nullptr;
        });

        return true;
    }();
    Q_UNUSED(installed)
}

void TerminalView::updateAccessibility()
{
    if (!QAccessible::isActive()) {
        return;
    }

    if (auto *accessible = dynamic_cast<TerminalAccessible *>(QAccessible::queryAccessibleInterface(this))) {
        accessible->update();
    }
}
}
