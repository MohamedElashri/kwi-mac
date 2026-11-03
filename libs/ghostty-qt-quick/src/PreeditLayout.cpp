// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PreeditLayout.h"

#include <Unicode.h>

#include <QBrush>
#include <QFont>
#include <QTextCharFormat>
#include <QVariant>
#include <algorithm>

namespace GhosttyQt::Quick
{
namespace
{
constexpr quint8 underlineStyle(QTextCharFormat::UnderlineStyle style)
{
    switch (style) {
    case QTextCharFormat::NoUnderline:
        return 0;
    case QTextCharFormat::SingleUnderline:
        return 1;
    case QTextCharFormat::WaveUnderline:
    case QTextCharFormat::SpellCheckUnderline:
        return 3;
    case QTextCharFormat::DotLine:
        return 4;
    case QTextCharFormat::DashUnderline:
        return 5;
    case QTextCharFormat::DashDotLine:
        return DashDotUnderline;
    case QTextCharFormat::DashDotDotLine:
        return DashDotDotUnderline;
    }

    return 1;
}

Cell preeditCell(const Frame &frame, const QString &text, qsizetype start, qsizetype end, const QList<QInputMethodEvent::Attribute> &attributes)
{
    Cell cell;
    cell.text = text.mid(start, end - start);
    cell.foreground = frame.foreground.rgb();
    cell.background = frame.background.rgb();
    cell.defaultBackground = false;
    cell.underline = 1;
    QColor underlineColor;

    for (const auto &attribute : attributes) {
        if (attribute.type != QInputMethodEvent::TextFormat || attribute.start >= end || qint64(attribute.start) + attribute.length <= start) {
            continue;
        }

        const QTextCharFormat format{qvariant_cast<QTextFormat>(attribute.value).toCharFormat()};

        if (format.hasProperty(QTextFormat::ForegroundBrush)) {
            cell.foreground = format.foreground().color().rgb();
        }

        if (format.hasProperty(QTextFormat::BackgroundBrush)) {
            cell.background = format.background().color().rgb();
        }

        cell.flags |= format.fontWeight() >= QFont::Bold ? Bold : 0;
        cell.flags |= format.fontItalic() ? Italic : 0;
        cell.flags |= format.fontStrikeOut() ? Strike : 0;
        cell.flags |= format.fontOverline() ? Overline : 0;

        if (format.hasProperty(QTextFormat::TextUnderlineStyle)) {
            cell.underline = underlineStyle(format.underlineStyle());
        } else if (format.hasProperty(QTextFormat::FontUnderline)) {
            cell.underline = format.fontUnderline() ? 1 : 0;
        }

        if (format.hasProperty(QTextFormat::TextUnderlineColor)) {
            underlineColor = format.underlineColor();
        }
    }

    cell.underlineColor = underlineColor.isValid() ? underlineColor.rgb() : cell.foreground;

    return cell;
}
}

Frame frameWithPreedit(const Frame &frame, const PreeditLayout &preedit)
{
    Frame result = frame;
    for (const auto &line : preedit.rows) {
        if (line.cell.y() < 0 || line.cell.y() >= result.rows.size())
            continue;
        auto &cells = result.rows[line.cell.y()];
        const int begin = line.cell.x();
        const int end = qMin(int(cells.size()), begin + int(line.cells.size()));
        if (begin > 0 && begin < cells.size() && cells[begin].width == 0) {
            cells[begin - 1].text.clear();
            cells[begin - 1].width = 1;
        }
        if (end < cells.size() && cells[end].width == 0) {
            cells[end].width = 1;
            cells[end].text.clear();
        }
        for (int x = qMax(0, begin); x < end; ++x)
            cells[x] = line.cells[x - begin];
    }
    result.cursor = preedit.caret;
    result.cursorVisible = preedit.caretVisible;
    result.cursorWideTail = false;
    result.cursorStyle = CursorStyle::Beam;
    result.cursorColor = preedit.caretColor;
    result.cursorBlinking = false;
    return result;
}

PreeditLayout layoutPreedit(const Frame &frame, const QString &text, const QList<QInputMethodEvent::Attribute> &attributes)
{
    PreeditLayout result;
    result.caret = {qBound(0, frame.cursor.x(), qMax(0, frame.columns - 1)), qBound(0, frame.cursor.y(), qMax(0, int(frame.rows.size()) - 1))};
    result.caretColor = frame.foreground;

    if (frame.cursor.x() < 0 || frame.cursor.x() >= frame.columns || frame.cursor.y() < 0 || frame.cursor.y() >= frame.rows.size()) {
        // There is no IME anchor while the live cursor is outside the viewport.
        result.caretVisible = false;

        return result;
    }

    if (text.isEmpty() || frame.columns <= 0 || frame.rows.isEmpty()) {
        return result;
    }

    qsizetype caretIndex{text.size()};
    bool explicitCaretColor{false};

    const auto cursorAttribute = std::ranges::find(attributes, QInputMethodEvent::Cursor, &QInputMethodEvent::Attribute::type);

    if (cursorAttribute != attributes.cend()) {
        caretIndex = qBound(qsizetype{0}, qsizetype{cursorAttribute->start}, text.size());
        result.caretVisible = cursorAttribute->length != 0;
        const QColor color{cursorAttribute->value.value<QColor>()};

        if (color.isValid()) {
            result.caretColor = color;
            explicitCaretColor = true;
        }
    }

    QPoint position{result.caret};
    qsizetype start{0};

    const auto clusters = graphemes(text);

    for (const auto grapheme : clusters) {
        const qsizetype end{start + grapheme.length};
        Cell cell{preeditCell(frame, text, start, end, attributes)};
        // Reserve a cell for an isolated combining mark. A one-column view clips wide clusters.
        cell.width = qBound(1, int(grapheme.width), qMin(2, frame.columns));

        if (position.x() + cell.width > frame.columns) {
            position = {0, position.y() + 1};
        }

        // Snap IME offsets inside surrogate pairs or combining clusters to the cluster boundary.
        if (caretIndex >= start && caretIndex < end) {
            result.caret = position;

            if (!explicitCaretColor) {
                result.caretColor = QColor::fromRgb(cell.foreground);
            }
        }

        if (result.rows.isEmpty() || result.rows.constLast().cell.y() != position.y()) {
            result.rows += PreeditRow{position, {}};
        }

        const int width{cell.width};
        result.rows.last().cells += cell;

        if (cell.width == 2) {
            cell.text.clear();
            cell.width = 0;
            result.rows.last().cells += cell;
        }

        position.rx() += width;
        start = end;
    }

    if (caretIndex == text.size()) {
        result.caret = {qMin(position.x(), frame.columns - 1), position.y()};

        if (!explicitCaretColor && !result.rows.isEmpty()) {
            result.caretColor = QColor::fromRgb(result.rows.constLast().cells.constLast().foreground);
        }
    }

    const int lastRow{qMax(result.caret.y(), result.rows.isEmpty() ? 0 : result.rows.constLast().cell.y())};
    const int shift{qMin(result.caret.y(), qMax(0, lastRow - int(frame.rows.size()) + 1))};
    result.caret.ry() -= shift;

    for (auto &row : result.rows) {
        row.cell.ry() -= shift;
    }

    result.rows.removeIf([&frame](const auto &row) {
        return row.cell.y() < 0 || row.cell.y() >= frame.rows.size();
    });

    return result;
}
}
