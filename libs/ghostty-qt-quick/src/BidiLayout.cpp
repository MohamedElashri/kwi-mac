// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "BidiLayout.h"

#include <algorithm>
#include <memory>
#include <unicode/ubidi.h>
#include <unicode/uchar.h>
#include <unicode/utf16.h>

namespace GhosttyQt::Quick
{
namespace
{
bool joinsAcross(const QString &text, int boundary)
{
    const auto *units = reinterpret_cast<const UChar *>(text.utf16());
    int before = boundary;
    int after = boundary;
    UChar32 left = 0, right = 0;
    int leftType = U_JT_TRANSPARENT;
    int rightType = U_JT_TRANSPARENT;
    while (before > 0 && leftType == U_JT_TRANSPARENT) {
        U16_PREV(units, 0, before, left);
        leftType = u_getIntPropertyValue(left, UCHAR_JOINING_TYPE);
    }
    while (after < text.size() && rightType == U_JT_TRANSPARENT) {
        U16_NEXT(units, after, text.size(), right);
        rightType = u_getIntPropertyValue(right, UCHAR_JOINING_TYPE);
    }
    return (leftType == U_JT_DUAL_JOINING || leftType == U_JT_LEFT_JOINING || leftType == U_JT_JOIN_CAUSING)
        && (rightType == U_JT_DUAL_JOINING || rightType == U_JT_RIGHT_JOINING || rightType == U_JT_JOIN_CAUSING);
}
}

bool BidiLayout::needsLayout(const QString &text)
{
    for (qsizetype index = 0; index < text.size(); ++index) {
        char32_t cp = text[index].unicode();
        // ASCII output never enters ICU or the complex shaping path.
        if (cp < 0x590)
            continue;
        if (text[index].isHighSurrogate() && index + 1 < text.size() && text[index + 1].isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(text[index], text[index + 1]);
            ++index;
        }
        const auto direction = QChar::direction(cp);
        if (direction == QChar::DirR || direction == QChar::DirAL || direction == QChar::DirAN || direction == QChar::DirRLE || direction == QChar::DirRLO
            || direction == QChar::DirLRE || direction == QChar::DirLRO || direction == QChar::DirPDF || direction == QChar::DirRLI
            || direction == QChar::DirLRI || direction == QChar::DirFSI || direction == QChar::DirPDI)
            return true;
    }
    return false;
}

void BidiLayout::update(const Frame &frame, bool enabled)
{
    m_rows = QVector<BidiRow>(frame.rows.size());
    if (!enabled)
        return;

    using Bidi = std::unique_ptr<UBiDi, decltype(&ubidi_close)>;
    for (int first = 0; first < frame.rows.size();) {
        int last = first;
        while (last + 1 < frame.rows.size() && last < frame.rowWrapped.size() && frame.rowWrapped[last])
            ++last;
        const QString prefix = first == 0 ? frame.bidiPrefix : QString();
        const QString suffix = last + 1 == frame.rows.size() ? frame.bidiSuffix : QString();
        bool complex = needsLayout(prefix) || needsLayout(suffix);
        for (int y = first; y <= last && !complex; ++y)
            for (const auto &cell : frame.rows[y])
                if (needsLayout(cell.text)) {
                    complex = true;
                    break;
                }
        if (!complex) {
            first = last + 1;
            continue;
        }

        QString text = prefix;
        QString key;
        const auto lengthKey = [&key](qsizetype length) {
            key += QChar(ushort(quint64(length) >> 16));
            key += QChar(ushort(length & 0xffff));
        };
        lengthKey(prefix.size());
        key += prefix;
        lengthKey(suffix.size());
        key += suffix;
        QVector<int> starts;
        QVector<QVector<int>> columns;
        for (int y = first; y <= last; ++y) {
            starts += int(text.size());
            QVector<int> units;
            key += QChar(ushort(frame.rows[y].size()));
            for (int x = 0; x < frame.rows[y].size(); ++x) {
                const auto &cell = frame.rows[y][x];
                key += QChar(ushort(cell.width));
                lengthKey(cell.text.size());
                key += cell.text;
                if (!cell.width)
                    continue;
                const QString value = cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
                text += value;
                units += QVector<int>(value.size(), x);
            }
            columns += units;
        }
        starts += int(text.size());
        text += suffix;
        if (const auto *cached = m_cache.object(key)) {
            for (int y = first; y <= last; ++y) {
                m_rows[y] = cached->at(y - first);
                m_rows[y].logicalCells = frame.rows[y];
            }
            first = last + 1;
            continue;
        }

        UErrorCode error = U_ZERO_ERROR;
        Bidi paragraph(ubidi_open(), ubidi_close);
        // Terminal columns have an LTR origin; individual RTL runs still follow UAX #9.
        // Resolve wrapped lines together, then apply line-specific whitespace resets.
        ubidi_setPara(paragraph.get(), reinterpret_cast<const UChar *>(text.utf16()), text.size(), 0, nullptr, &error);
        QVector<BidiRow> resolved;
        for (int y = first; y <= last && U_SUCCESS(error); ++y) {
            const int index = y - first;
            if (starts[index] == starts[index + 1]) {
                resolved += BidiRow{};
                continue;
            }
            Bidi line(ubidi_open(), ubidi_close);
            ubidi_setLine(paragraph.get(), starts[index], starts[index + 1], line.get(), &error);
            QVector<int32_t> visual(starts[index + 1] - starts[index]);
            ubidi_getVisualMap(line.get(), visual.data(), &error);
            if (U_FAILURE(error))
                break;
            BidiRow row;
            row.joinBefore = joinsAcross(text, starts[index]);
            row.joinAfter = joinsAcross(text, starts[index + 1]);
            const auto &cells = frame.rows[y];
            row.logicalToVisual = QVector<int>(cells.size(), -1);
            row.visualToLogical = QVector<int>(cells.size(), -1);
            row.levels = QVector<quint8>(cells.size(), 0);
            QVector<int> representatives(cells.size(), -1);
            for (int unit = 0; unit < columns[index].size(); ++unit) {
                const QChar c = text[starts[index] + unit];
                const auto direction = c.direction();
                const bool control = c == QChar(0x061c) || c == QChar(0x200e) || c == QChar(0x200f) || direction == QChar::DirLRE || direction == QChar::DirRLE
                    || direction == QChar::DirLRO || direction == QChar::DirRLO || direction == QChar::DirPDF || direction == QChar::DirLRI
                    || direction == QChar::DirRLI || direction == QChar::DirFSI || direction == QChar::DirPDI;
                const int source = columns[index][unit];
                if (!control && representatives[source] < 0)
                    representatives[source] = unit;
            }
            int destination = 0;
            for (const int unit : std::as_const(visual)) {
                const int source = columns[index][unit];
                if (representatives[source] >= 0 && unit != representatives[source])
                    continue;
                if (row.logicalToVisual[source] >= 0)
                    continue; // Combining marks and surrogate halves share their base cell.
                for (int part = 0; part < qMax(1, int(cells[source].width)) && source + part < cells.size(); ++part) {
                    row.logicalToVisual[source + part] = destination;
                    row.visualToLogical[destination++] = source + part;
                    row.levels[source + part] = ubidi_getLevelAt(line.get(), unit);
                }
            }
            // A wide glyph may leave a zero-width spacer at the wrap margin.
            // It has a physical cell but contributes no paragraph text. Keep
            // that padding at the visual margin without adding a fake space.
            for (int source = 0; source < cells.size(); ++source) {
                if (row.logicalToVisual[source] < 0 && !cells[source].width) {
                    row.logicalToVisual[source] = destination;
                    row.visualToLogical[destination++] = source;
                }
            }
            if (destination != cells.size()) {
                error = U_INTERNAL_PROGRAM_ERROR;
                break;
            }
            resolved += row;
        }
        if (U_SUCCESS(error)) {
            m_cache.insert(key, new QVector<BidiRow>(resolved), qMax(1, int(key.size())));
            for (int y = first; y <= last; ++y) {
                m_rows[y] = resolved[y - first];
                m_rows[y].logicalCells = frame.rows[y];
            }
        }
        first = last + 1;
    }
}

QPoint BidiLayout::visualCell(QPoint cell) const
{
    if (cell.y() >= 0 && cell.y() < m_rows.size()) {
        const auto &map = m_rows[cell.y()].logicalToVisual;
        if (cell.x() >= 0 && cell.x() < map.size())
            cell.setX(map[cell.x()]);
    }
    return cell;
}

QPoint BidiLayout::logicalCell(QPoint cell) const
{
    if (cell.y() >= 0 && cell.y() < m_rows.size()) {
        const auto &map = m_rows[cell.y()].visualToLogical;
        if (cell.x() >= 0 && cell.x() < map.size())
            cell.setX(map[cell.x()]);
    }
    return cell;
}

QRegion BidiLayout::visualRegion(const QRegion &logical) const
{
    QRegion result;
    for (const QRect &rect : logical)
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x)
                result += QRect(visualCell({x, y}), QSize(1, 1));
    return result;
}

Frame BidiLayout::visualFrame(const Frame &logical) const
{
    Frame result = logical;
    for (int y = 0; y < qMin(m_rows.size(), logical.rows.size()); ++y) {
        const auto &map = m_rows[y].visualToLogical;
        if (map.isEmpty())
            continue;
        QVector<Cell> cells;
        cells.reserve(map.size());
        for (const int source : map)
            cells += logical.rows[y][source];
        result.rows[y] = cells;
    }
    result.cursor = visualCell(logical.cursor);
    return result;
}
}
